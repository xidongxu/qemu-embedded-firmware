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

### 4. 仪器化 memcpy/memset
- 现状：bulk copy 不查 shadow，对 poison 区的大块拷贝/跨区拷贝漏报。
- 修法：`-Wl,--wrap=memcpy` / `__wrap_memset`（或 hook），整段做 `kasan_check_range`。

### 5. quarantine（或 alloc/free 调用栈）
- 现状：UAF 只在"内存被复用前"可检测；TLSF 复用旧块后，经旧指针的访问不再报。
- 方案 A：quarantine——freed 对象延迟归还分配器，保持 poison 一段时间，延长 UAF 检测窗口。
- 方案 B：alloc/free 调用栈记录——报 UAF/double-free 时同时给出分配点与释放点栈（类似 Linux `CONFIG_KASAN_STACK`）。

## 三、覆盖范围与工程项（P2，按场景）

### 6. 栈红区 / 全局红区
- 现状：栈与全局变量完全不查（`-fsanitize=kernel-address` 路线固有限制 + 库只维护堆）。
- 需编译器侧 + startup 配合；an505 上 TZ 板 CPU 写 shadow 曾卡 BusFault，先验证可行性。

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
