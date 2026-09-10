# kasan 改进待办（与 Linux KASan 对齐）

> 记录日期：2026-09-09。来源：本库与 Linux KASan（generic）+ slab 的逐项对比。
> 用法：按优先级逐项推进，每完成一项打勾并记录 commit。

## 一、正确性缺陷（P0，先修）

### 1. partial-granule 尾部 4 字节漏报 ✅ 已完成
- 现象：shadow 按 8 字节粒度，而 TLSF 块尺寸是 4 字节对齐。块尺寸非 8 倍数时，最后 4 字节（含下一块 header）被整 granule unpoison，**尾部 ≤4 字节的越界写漏报**。8 对齐的块不受影响。
- 修法：引入 partial 编码 `0x01–0x07`（前 N 字节可访问）/ `0xf1–0xf7`（前 N 字节 poison），`kasan_poison` / `kasan_unpoison` / `kasan_check` 三处同步改。
- 验证：host 加 `test_partial_granule`（合成 20 字节区间 + 直调 noabort 钩子）；QEMU 加 case7（`malloc(20)` 后写 `a[20]`）。
- 完成：host reports=7 全过；QEMU case7 shadow=0x04 捕获、case1/case6 回归全过。顺带修复了 realloc 缩容头部 4 字节的误 poison（旧实现会把新 user 区尾部 4 字节误标 poison 造成假阳性）。

### 2. kasan_report_pc 不准 ✅ 已完成
- 现象：`kasan_report` 里取 `__builtin_return_address(0)`，拿到的是 `kasan_check` 调用 report 之后的地址（比真实访问点深 2 层），且 `-O2` 下可能被内联。
- 修法：在 noabort 钩子层取 `__builtin_return_address(0)`（即真实访问点）逐级传入 `kasan_check` → `kasan_report`。
- 验证：QEMU 读 `kasan_report_pc`，应落在 main 里触发访问的那条插桩指令处（而非 `kasan_check` 内部）。
- 完成：`kasan_report`/`kasan_check` 加 `pc` 参数，钩子宏捕获调用点；double-free/bad-free 报告的 pc 取 `kasan_free`/`kasan_realloc` 的调用者。QEMU case1 实测 `kasan_report_pc=0x100000b9`，gdb `info symbol` 解析为 `main + 73`；host 加 `kasan_report_pc != 0` 断言。

## 二、能力差距（P1，高价值）

### 3. 语义化 poison 值 + 报告可读化 ✅ 已完成
- 现状：shadow 只有 `0x00` / `0xff`，报告只给 type(load/store/double/bad)，无法说明"这是 UAF 还是越界、红区在哪"。
- 目标：引入语义值（区分 freed / slab redzone / bad-free 等，参考 Linux 的 0xFA/0xFB/0xF8/0xFC 等），报告时翻译成人类可读描述，并 dump 故障地址周围的 shadow 状态。
- 完成：`0xFA`=freed（UAF）、`0xFB`=redzone（header/free 块/未用 arena）；`kasan_poison` 拆出 `kasan_poison_as(addr,len,value)`，heap_init/free/realloc 分别用 redzone/freed。新增 `kasan_shadow_name()`、`kasan_shadow_dump(addr,out,count)`、marker `kasan_report_cause`（0 未知 1 redzone 2 freed 3 partial 4 通用 poison）+ `kasan_report_shadow_dump[16]`。验证：host reports=8；QEMU case1 shadow=0xFB cause=1、case2 shadow=0xFA cause=2。

### 4. 仪器化 memcpy/memset ✅ 已完成
- 现状：bulk copy 不查 shadow，对 poison 区的大块拷贝/跨区拷贝漏报。
- 修法：`-Wl,--wrap=memcpy,--wrap=memset,--wrap=memmove` + 库内 `__wrap_*` 拦截器，整段做 `kasan_check`；TLSF 后端 realloc 改用裸循环拷贝（避免 `tlsf_realloc` 内部 libc memcpy 被 wrap 造成假阳性）。
- 关键坑（必读）：`-O2` 会把拦截器里的填字节/拷贝循环优化回 `memcpy/memset` 调用，又被 `--wrap` 导回 `__wrap_*` 自身 → **无限递归 → 栈下溢 → BusFault**。修法：kasan 库目标加 `-fno-builtin -fno-tree-loop-distribute-patterns`（CMakeLists 已加并注释）。
- 顺带修复 `kasan_report` 的 shadow/cause 取自"访问起始地址"而非"实际越界字节"的 bug（新增 `fault_addr` 参数，shadow/cause/dump 按越界字节计算；`kasan_report_addr` 仍保留访问地址语义）。
- 完成：host reports=10 全过（新增 `test_wrap_copy` 断言 cause=1）；QEMU case8（memcpy 读越界 shadow=0xFB cause=1）、case9（memset 写越界 shadow=0xFB cause=1）捕获，case1–7 全回归过。

### 5. quarantine（或 alloc/free 调用栈）✅ 已完成
- 现状：UAF 只在"内存被复用前"可检测；TLSF 复用旧块后，经旧指针的访问不再报。
- 方案 A：quarantine——freed 对象延迟归还分配器，保持 poison 一段时间，延长 UAF 检测窗口。
- 方案 B：alloc/free 调用栈记录——报 UAF/double-free 时同时给出分配点与释放点栈（类似 Linux `CONFIG_KASAN_STACK`）。
- 完成（方案 A 为主 + B 的一级调用点子集）：
  - **quarantine**：`kasan_free` 不再立即归还分配器，而是入 FIFO（`KASAN_QUARANTINE_BYTES` 按总字节限流，默认 8KB；`KASAN_QUARANTINE_MAX` 按条数，默认 64），freed 块保持 0xFA；超限时释放最旧块。`kasan_quarantine_drain()` 可强制清空；`kasan_heap_init` 重置。注意 realloc 迁移的旧块仍由后端立即归还（不进 quarantine）。
  - **alloc/free 调用点**：记录表每条加 `alloc_pc`/`free_pc`（20 字节/条），报告新增 `kasan_report_alloc_pc`/`kasan_report_free_pc`，UAF/double-free 时按 freed 记录反查填充。
  - **已知限制**：记录表复用已 freed 槽位后，旧指针的调用点信息会丢失（UAF 仍靠 shadow 0xFA 捕获，但 alloc/free pc 读 0）；完整多层调用栈回溯（stack depot）成本高，暂不做。
  - 验证：host reports=12（test_quarantine：隔离不重用 + 超限释放后复用 + UAF/double-free 调用点断言）；QEMU case10（free 后同尺寸 malloc 不复用，旧指针写仍 trap，shadow=0xFA cause=2）、case2/3 读 alloc_pc/free_pc 非 0，case1-9 全回归过。

## 三、覆盖范围与工程项（P2，按场景）

### 6. 栈红区 / 全局红区 ⛔ 已验证不可行（kernel-address 路线）
- 现状：栈与全局变量完全不查（`-fsanitize=kernel-address` 路线固有限制 + 库只维护堆）。
- **2026-09-10 可行性验证结论（arm-none-eabi-gcc 15.3.1 + `-fsanitize=kernel-address` 实测）**：
  - **栈访问完全不插桩**：`uint32_t a[4]` 的 `a[0]`（边界内）、`a[4]`（常量越界）、
    `a[i]`（动态下标）编译后全是普通 `str`，`nm -u` 零 `__asan` 符号。编译器层面堵死，
    栈红区**无解**。
  - **全局访问只插桩「越界/无法静态证明安全」的**：`g_arr[10]`（常量越界）会生成
    `__asan_store4_noabort`；`g_arr[0]`（边界内常量）被跳过（静态证明在对象范围内）。
  - 但 kernel-address **不生成全局 redzone padding，也不生成 `__asan_register_globals`**：
    全局变量在 `.bss` 里紧挨着（`g_array` 后直接 `g_scalar`，无间隙）。没有 redzone 可毒，
    越界检查命中下一个变量的合法地址（shadow=0）→ 放行。
  - 对照：普通 `-fsanitize=address` 生成 `__asan_init`/`__asan_register_globals`/
    `__asan_stack_malloc_1`（有全局 + 栈 redzone），但该路线 shadow 基址硬编码
    [0x20000000,0x40000000)、且 an505 TZ 板写 shadow 曾卡 BusFault（见仓库历史）。
- 结论：**维持 kernel-address + 堆红区为定论**；栈/全局红区需换普通 address 路线才可能，
  代价与历史坑不划算。全局越界的「手动 padding + 手动毒化」可以做，但属用户手工配合，
  库不自动支持。

### 7. 多区域覆盖
- 现状只覆盖 `KASAN_REGION` 一个区，区外（栈/全局/外设）`shadow_of` 返 0 直接放行。

### 8. 其他工程项
- 记录表压 8 字节/条（state 塞进 ptr 低 3 位，4096 条 48KB→32KB）。
- 报告 sink 接 UART/tracer（现在只有 gdb marker）。
- 无锁：记录表/shadow 更新在多任务/中断下有竞态（需要时再加临界区）。
- tests 矩阵脚本化（一次跑 QEMU case 1–6 判 PASS）+ host ctest 常规化。

## 已对齐（不需要做）
- 编译器插桩模型（每次访存查 shadow）、shadow 映射（addr>>3 + base）、堆越界/UAF/double-free 即时捕获、可插拔分配器后端。

## 明确不做
- SW_TAGS / HW_TAGS（软件/硬件标签）：Cortex-M 上基本不现实。
- ksize 红区：TLSF 是 4 字节粒度近似精确分配，请求与实际块差距 ≤3 字节，此点当前不亏。

## 暂缓（其他问题处理完后再考虑）

### 完整多层调用栈回溯（UAF/double-free 附整条调用链）
- 现状（P1#5 已做）：记录表存 `alloc_pc` / `free_pc` **一级**返回地址，离线用
  addr2line / gdb 解析成「函数 + 行号」。这一级几乎零成本，能覆盖多数调试场景。
- 升级成「整条调用链」（A→B→C→malloc 三层全记）时的成本点（2026-09-10 分析结论）：
  1. **收集多层 LR 难**：Cortex-M/Thumb 默认省略帧指针，`__builtin_return_address(n)`
     在 n≥1 不可靠。要么全局 `-fno-omit-frame-pointer`（代码膨胀、寄存器压力↑、
     混合编译选项会断链）；要么运行时栈展开器（解析 `.ARM.exidx`，几 KB 代码 +
     每函数一条展开数据，还要处理 PSP/MSP/EXC_RETURN）。
  2. **存储链贵**：每层 4 字节，8 层 = +28B/条 → 记录表从 80KB 涨到 ~196KB；
     用 stack depot 去重则引入哈希表 + 引用计数 + 锁 + 独立内存池（又回到「用谁的
     malloc 给 depot 分内存」）。
  3. addr2line 符号解析**不是**成本点——host 离线做，免费（现状已在这么做）。
- 若真要做，走轻量路线（非 depot）：`-fno-omit-frame-pointer` + 手写 ~10 行帧指针
  链遍历（沿 r7/r11 链读回每层保存的 LR），运行时与代码量都小，代价是全局代码
  膨胀几个百分点。
- 结论：暂不做；等「同一函数多处分配 / 多层泛型 allocator 包装」这类复杂调用链
  场景真实出现，再按上述轻量路线实施。
