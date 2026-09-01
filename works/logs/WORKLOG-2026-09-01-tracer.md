# WORKLOG 2026-09-01 — tracer: Cortex-M fault dump 库

## 需求
实现新库 `tracer`：cortex-m 系列（M3 → M85）fault dump。要求：
- 兼容 M 系列全系（M0/M0+/M3/M4/M7/M23/M33/M55/M85）
- 文件尽量简洁、单目录（方便并入其他库）
- CMake 构建

替代原先粗略的 `libutils/fault-dump/`（多目录：cpu/inc/src/sys）。

## 交付结构（libutils/tracer/，4 文件）
```
CMakeLists.txt    project(tracer C ASM); add_library(tracer STATIC tracer.c tracer_vectors.S)
tracer.h          配置宏(TRACER_PRINTF/TRACER_STACK_DEPTH)、tracer_exc_frame_t、
                  tracer_core_frame_t、tracer_fault_t、API + weak hooks
tracer.c          SCB 裸地址、内存映射符号(_stext/Image$$/__section)、帧解码、
                  调用栈 BL/BLX 扫描、trap
tracer_vectors.S  入口宏：push {r4-r11} → EXC_RETURN bit2 判 PSP/MSP → C handler
```

## 设计要点
- 无 CMSIS/RTOS/printf 依赖：SCB 0xE000ED00 + 8 字异常帧在 M3..M85 一致；TrustZone/FPU/MVE 仅编译期。
- 强符号 NMI/HardFault/MemManage/BusFault/UsageFault/SecureFault_Handler 覆盖 startup weak，**无需改 startup**（startup 的 HardFault 已还原为 weak `b .`）。
- **必须 whole-archive 链接**：静态库"引用才拉入"，而向量表引用已被 weak 满足 → `-Wl,--whole-archive tracer -Wl,--no-whole-archive`。
- 调用栈扫描：活动栈上找 Thumb-2 BL/BLX 返回地址（PC=ret-1-4，校验回 .text 段）。

## 验证过程（QEMU mps2-an505, cortex-m33）
1. main.c TEMP 触发 `*(volatile uint32_t*)0xDEADBEEF = 0x12345678`（BusFault）。
2. 首版转储 bug：
   - SP 显示 MSP（0x80FFFF28）→ **EXC_RETURN 位判断错**（`&0x2` 应为 `&0x4`，bit2=PSP）。修后 SP=0x80215080（任务栈 PSP）。
   - CFSR 分解错（MMFSR=8200/BFSR=0000）→ 位宽错（`&0xFFFF`/`>>16` 应为 `&0xFF`/`>>8`）。修后 MMFSR=00/BFSR=82。
   - 异常类型"IRQn (IPSR=0)"→ 线程模式 fault 时 xPSR.IPSR 本来就 0（只 T 位），**正常**；改为用 CFSR/HFSR 判 fault 类型。
3. 修正后转储（完全正确）：
   ```
   Exception: BusFault (IPSR=0)
   EXC_RETURN: 0xFFFFFFFD [Thread mode, PSP, Secure]
   R2=12345678（value）R3=DEADB000（bad 地址）SP=80215080（任务栈）
   PC=100018C8（main_task_entry fault 点）xPSR=01000000（T，IPSR=0 正常）
   CFSR=00008200 MMFSR=00 BFSR=82 UFSR=0000
   HFSR=40000000（FORCED：BusFault 未使能 escalate 成 HardFault）
   BFAR=DEADBEEF [VALID]
   Call stack: 100049DA 1000589E ...
   ```
4. HMP 交叉验证（`works/tools/hmp_regs.py`）：PSP=0x80215080 处内存即异常帧（R0=0,R1=01010101,R2=12345678,R3=DEADB000,...），HMP 当前 XPSR IPSR=3=HardFault 与转储一致。
5. 移除 TEMP 触发 → boot 干净：`Tracer: Cortex-M fault dump ready` + `text [10000000-101CD48C]` + `stack [80FFEFF0-80FFFFF0]`，进入 pjlib 初始化无 fault。

## 经验/坑
- **EXC_RETURN bit2=0x4 是 PSP**（不是 bit1）。0xFFFFFFFD = Thread+PSP+Secure；bit3=Thread/Handler。
- **线程模式 fault 时异常帧 xPSR.IPSR=0 是正常的**（不是 bug），别被误导。
- **CFSR = MMFSR[7:0] | BFSR[15:8] | UFSR[31:16]**（UFSR 单独 16 位寄存器 @0x2A）。
- 精确 BusFault 未被使能时 escalate 成 HardFault：HFSR=0x40000000 FORCED，根因看 CFSR.BFSR。
- HMP `info registers` 显示的是**当前 handler 状态**（IPSR=3, PC 在 tracer），异常帧要读 PSP 内存（`xp`）。
- 应用必须手动 `tracer_init()` + include path；init 打印 text/stack 范围对日后 addr2line 定位极有用。

## 全面替换 fault-dump（同日二次会话）

- **tracer 增加 2 个主动 API**：`tracer_dump_callstack()`（从当前 SP 扫描 BL/BLX，替代 `fault_dump_callstack` 主动用法）、`tracer_trigger_unalign()`（设 CCR.UNALIGN_TRP + 未对齐读触发 UsageFault，替代 `fault_dump_unalign`）。fault handler 的扫描逻辑抽成共享 `tracer_scan_callstack()`。
- **FreeRTOS 任务名 hook**：应用定义 `tracer_on_fault()` 打印 fault 时当前任务（替代 fault-dump 的 psp_stack_parser 机制）。
- **tracer_vectors.S → tracer_vectors.s**（小写）：CubeMX/Makefile 只有 `.s` 汇编规则；文件是纯汇编器宏（无 cpp 指令），改名安全。
- **迁移 5 个工程**：mps2-an505 FreeRTOS/BareMetal（CMake）+ stm32f405rg FreeRTOS/BareMetal/threadx（Makefile）。
  - mps2：main.c 换 tracer API，CMake 移除 `utils` 链接点（`utils` 聚合库原本就是 fault-dump 定义：`add_library(utils STATIC fault-dump源码)`）——FreeRTOS/BareMetal/threadx/Drivers 4 处。
  - stm32：Makefile C_SOURCES/ASM_SOURCES/C_INCLUDES 换 tracer；`stm32f4xx_it.c` 的 NMI/MemManage/BusFault/UsageFault_Handler 加 `TRACER_WEAK`（`__attribute__((weak))`）让 tracer 强符号覆盖（CubeMX 默认强符号会冲突）；stm32 ld 已有 `_stext/_etext/_sstack/_estack`（无需改）。stm32f405=M4，验证 tracer 符号：HardFault_Handler=080001F4 等（tracer 接管）。
  - iar/mdk 分支：tracer 暂无 IAR/MDK 汇编，移除 fault-*.s（退回 startup weak handler）；gcc 完整支持。
- **删除 libutils/fault-dump/**（目录 + libutils/CMakeLists.txt 的 add_subdirectory）。

## 验证（fault-dump 删除后）
- mps2 FreeRTOS：boot 干净（Tracer ready + text/stack，进入 pjlib）。
- mps2 BareMetal：test 链 → `tracer_dump_callstack`（SP=380FFFE8: 10000882 100008BE）→ `tracer_trigger_unalign` → UsageFault dump（UFSR=0100 UNALIGN、PC=10001E5A、HFSR=40000000 FORCED）。
- stm32 FreeRTOS/BareMetal：make tool=gcc 链接成功，nm 确认 tracer 强符号接管 4 个 fault handler。

## MDK / IAR 兼容（同日三会话）

用户问 tracer 是否兼容 MDK/IAR。补齐并实测验证：

- **tracer.c 纯 C 化**：移除全部内联汇编（`mrs msp/psp` 读取 + `tracer_dump_callstack` 的 `mov sp`）：
  - f.sp 改由 exc_frame 推导（MSP 情况 -sizeof(core_frame)=32；入口汇编已定 PSP/MSP）——**更准**（fault 入口真实 SP，旧法在 C 函数里读 MSP 被 handler 调用栈抬高 0xA0）。
  - `tracer_dump_callstack` 用局部变量地址近似 SP（`uint32_t sp; sp=(uint32_t)&sp;`）。
  - weak hooks 用 `TRACER_WEAK` 宏：IAR 用 `#pragma weak`（`__weak` 需 --eec，很多工程不开）；GCC/ARMCC 用 `__attribute__((weak))`。
- **新增汇编入口**（三工具链各一，勿混用）：
  - `tracer_vectors.s`（GCC/Clang/armclang）
  - `tracer_vectors_iar.s`（IAR iasmarm）——**展开写 6 个 handler 不用宏**（IAR 宏参数不能作标号名）；宏名在 MACRO 前等语法坑已踩。
  - `tracer_vectors_mdk.s`（MDK armasm，ARMCC5；ARMCC6 用 GCC 版）
- **实测编译**（用本机 ARM_IAR 9.50 + ARMCC506）：
  - IAR：iccarm 编 tracer.c ✓ + iasmarm 编 tracer_vectors_iar.s ✓
  - MDK：armasm 编 tracer_vectors_mdk.s ✓ + armcc 编 tracer.c ✓（**ARMCC5 需 `--c99`**，否则 `inline` 未定义）
  - GCC：mps2 QEMU 回归 ✓（boot + BareMetal fault，SP 推导正确）
- **stm32 Makefile**：iar 分支加 `tracer_vectors_iar.s`，mdk 分支加 `tracer_vectors_mdk.s`（原来只留 startup weak handler）。

## 关联
- fault-dump 已完全移除；所有错误处理统一走 tracer。
- 本次提交文件：libutils/tracer/*（新增，含改名 .s）, libutils/CMakeLists.txt, 各板 main.c/Makefile/CMakeLists, stm32f4xx_it.c, startup_ARMCM33.s(还原 weak), works/tools/hmp_regs.py。

---

# 追加：exidx 精确回溯 / 工具链守卫 / 帧指针链实测 / 业界方案全景（同日四~七会话）

## 一、背景：为什么还要"加强"

扫描法（BL/BLX 启发式）在 **-O2 下 RTOS 任务栈实测 0 帧**：
- 内联：无 BL 指令，栈上无对应返回地址；
- 尾调用优化：末尾用 B 而非 BL，不压 LR；
- 返回地址走寄存器：LR 不压栈。

结论：不加强"基本用不了"。业界精确方案是 **EHABI `.ARM.exidx` 展开表 + libgcc `_Unwind_Backtrace`**。

## 二、`.ARM.exidx` 精确回溯（已实现 + 已验证）

### 实现
- `tracer.h`：新增 `TRACER_USE_EXIDX`（默认 0）；说明注释（fault 路径仍用扫描）。
- `tracer.c`：
  - `TRACER_HAVE_EXIDX` 守卫：`TRACER_USE_EXIDX && (__GNUC__||__clang__)`（IAR/ARMCC5 无 `<unwind.h>` → 静默回退扫描）。
  - `_Unwind_Backtrace` 回溯块：`tracer_unwind_cb` + `tracer_backtrace_exidx(buf,size)`。
  - `tracer_get_callstack` / `tracer_dump_callstack` 三级分支：**exidx > FP > scan**。
- `boards/mps2-an505/FreeRTOS/CMakeLists.txt`：`target_compile_definitions(tracer PRIVATE TRACER_USE_EXIDX=1)`（app 源码保留 `-funwind-tables`）。

### 两个关键坑（都实测踩过）
1. **宏必须定义在 tracer 库目标**（`target_compile_definitions(tracer ...)`），定义在 app 目标不传到单独编译的 tracer.c → 第一次改后 main.c 直调 n=2 是**假象**（app 帧有表、库帧无表）。
2. **tracer 库自身也必须 `-funwind-tables`** → 否则 `_Unwind_Backtrace` 从库内当前帧开始无展开信息 → 0 帧。加 `target_compile_options(tracer PRIVATE -funwind-tables)` 后从库内调用也正确。

### 验证（QEMU mps2-an505, -O2）
```
get_callstack n=3: 100BFADE 100018D6 10006204
addr2line → tracer_get_callstack / main_task_entry / prvTaskExitError（全对）
```
同位置扫描法 n=0 → exidx 彻底解决 -O2 问题。

## 三、`-funwind-tables` 归属（放 tracer 自己的 CMakeLists）

- `-funwind-tables` 是 **tracer 库自身**生成 `.ARM.exidx` 所需的编译属性 → 收进 `libutils/tracer/CMakeLists.txt`（`target_compile_options(tracer PRIVATE -funwind-tables)`，近无害，使用方无需重复）。
- `TRACER_USE_EXIDX` 是**行为开关** → 使用方 opt-in（`target_compile_definitions(tracer PRIVATE TRACER_USE_EXIDX=1)`，跨目录对 target 生效，不依赖 add_subdirectory 顺序）。
- app 自身源码也要 `-funwind-tables`，回溯链才能延伸到 app 帧（使用方责任，无法由库代劳）。

## 四、CMakeLists 工具链防卫

`libutils/tracer/CMakeLists.txt` 按 `CMAKE_C_COMPILER_ID` 自动选汇编入口：

| CMAKE_C_COMPILER_ID | 汇编文件 | -funwind-tables |
|---|---|---|
| GNU / Clang / ARMClang | `tracer_gnugcc.s` | ✅ |
| IAR | `tracer_iccarm.s` | ❌（不支持该选项） |
| ARMCC | `tracer_armcc.s` | ❌ |
| 其他 | `message(FATAL_ERROR)` 列支持清单 | — |

汇编文件名最终定为 `tracer_gnugcc.s` / `tracer_iccarm.s` / `tracer_armcc.s`。

## 五、帧指针链（TRACER_USE_FP）：Cortex-M 上基本不可用（实测结论）

### 实现
- `tracer.h`：`TRACER_USE_FP`（默认 0）。
- `tracer.c`：`TRACER_HAVE_FP` 守卫 + `tracer_backtrace_fp`（`__builtin_return_address(N)` 固定深度展开，N 需**编译期常量**，循环变量报 `invalid argument`；text 段校验防垃圾/防崩）。

### 三个硬限制（都实测踩过）
1. **Cortex-M = Thumb，GCC/armclang 帧指针是 r7，不是 AAPCS r11**；序言是批量 `push {r4..r11, lr}` + 局部偏移，**每帧偏移随压栈寄存器数变化，无固定 `[fp]=prev,[fp+4]=lr` 链可手动遍历**。手写 `mov r11` 读到普通寄存器垃圾 → 读 `0x11111115` → **BusFault**（BFAR=0x11111115 实测）。
2. **`__builtin_return_address(1)`+ 在 ARM 上不可靠**（GCC 文档即如此）；实测 **noinline 也最多 1 帧**（最内层调用者，如 main_task_entry 内返回点 `10001904`，addr2line 正确）。
3. **RTOS 任务栈在 ucHeap（bss 0x8020xxxx），远低于主栈 `_sstack=0x80ffeff0`**；任何用 `TRACER_STACK_BASE` 作 FP 下界的检查都会把任务栈内 FP 拒掉。

### 结论
- **Cortex-M 上用 exidx**；FP 链仅对 **A32（r11 标准链）或 IAR** 有意义，tracer 里 opt-in 保留并注释说明局限。
- 顺便暴露：fault path 扫描法在 RTOS 任务栈（低于主栈）下也有同样的 `TRACER_STACK_BASE` clamp 隐患（scan 被抬到主栈底 → 0 帧），后续可改为"scan 不 clamp，仅以 `tracer_stack_limit()` 为上界 + 单调性保护"。

## 六、业界栈回溯方案全景 & 我们的实现状态

### 业界常用方案对比

| 方案 | 原理 | 确定性 | 编译依赖 | 运行时开销 | 典型使用者 |
|---|---|---|---|---|---|
| 栈扫描 | 扫栈找像返回地址的值 + 反汇编确认 BL/BLX | 启发式，误报/漏报 | 无 | 低（O(栈长)） | 裸机 DIY、早期 Linux ARM |
| 帧指针链 | 专用 FP 寄存器串帧，`fp→{lr, fp_next}` | 高 | `-fno-omit-frame-pointer` | 极低 | Linux dump_stack、GDB、RTOS 教程 |
| 展开表 | `.ARM.exidx`/`.eh_frame`，运行时按规则算上一帧 | **精确** | `-funwind-tables` | 中 | GCC `_Unwind_Backtrace`、Android libunwind/tombstone、Linux DWARF/ORC |
| 影子栈 | 返回地址复制到受保护区 | 精确+抗篡改 | 编译器插桩（RISC-V Zicfiss/ARMv8.1-M 可选） | 低 | 安全关键系统 |
| 离线解析 | 只记 PC/LR/SP/CFSR + 原始栈，事后用 ELF 符号展开 | 事后精确 | 保留 debug 符号 | 近零 | SEGGER SystemView、IAR C-SPY、Keil μVision |

### 我们已实现

| 方案 | 状态 | 入口 | 备注 |
|---|---|---|---|
| 栈扫描（BL/BLX） | ✅ 已实现（兜底） | 默认（两宏均 0） | M0..M85 / IAR / ARMCC5 可用，零依赖；-O2 下可能 0 帧 |
| `.ARM.exidx` 展开表 | ✅ 已实现 + 验证 | `TRACER_USE_EXIDX=1` | -O2 精确，GCC/armclang，QEMU n=3 全对 |
| 帧指针链 | ⚠️ 已实现但 Cortex-M 受限 | `TRACER_USE_FP=1` | Thumb 下仅最内层 1 帧；A32 / IAR 才有完整意义 |

### 未实现方案 & 实现思路

| 方案 | 实现思路（落地步骤） |
|---|---|
| **离线解析（crash dump → 符号）** | ① fault 时把 PC/LR/SP/CFSR + 原始栈区（如 256B hex）随 dump 落 flash/串口（tracer 已有帧打印，加"原始栈 hex dump"）；② 事后脚本 `works/tools/tracer_decode.py`：用 pyelftools 读 ELF 符号表 + `.ARM.exidx` 离线展开，把 PC 数组自动转成 `函数名 + 行号` 调用链。tracer 的 `tracer_get_callstack()` 已能取 PC 数组，接落盘即可。**这是无 exidx 工具链（IAR/ARMCC5）获得精确回溯的可行路径**。 |
| **影子栈** | 需编译器插桩或硬件（RISC-V Zicfiss、ARMv8.1-M 可选 PAuth），主流 Cortex-M 工具链不生成 → 非本库范畴，仅作认知储备。 |
| **`-finstrument-functions` 动态轨迹** | 运行时记录函数进出环形 buffer（每调用 2 个回调），开销大、侵入强；只在需要"动态调用轨迹/覆盖"而非"静态回溯"时用，与 fault dump 定位目标不同。 |

### 使用建议（最终结论）
- **Cortex-M + GCC/armclang**：`TRACER_USE_EXIDX=1` + 全工程 `-funwind-tables`（mps2 FreeRTOS 已配置，boot 干净）。
- **IAR / ARMCC5**（无 exidx）：扫描法兜底；要精确 → 走"离线解析"（待实现）。
- 后续优先做 `tracer_decode.py` 离线符号工具，把 fault dump 的 PC 数组 + 原始栈自动转可读调用链。

## 本次（本会话）变更文件
```
M libutils/tracer/CMakeLists.txt   # -funwind-tables 收进库自身 + 工具链守卫 + FP 说明
M libutils/tracer/tracer.c         # exidx 回溯块 + FP 链(__builtin_return_address) + 注释清理
M libutils/tracer/tracer.h         # TRACER_USE_EXIDX / TRACER_USE_FP 宏 + 说明
M works/logs/WORKLOG-2026-09-01-tracer.md  # 本文档
```
（mps2 FreeRTOS CMakeLists 已在上一 commit 配好 `TRACER_USE_EXIDX=1`；`OARD=mps2-an505/` 误建目录已删）

建议提交：
```bash
git add libutils/tracer/CMakeLists.txt libutils/tracer/tracer.c libutils/tracer/tracer.h works/logs/WORKLOG-2026-09-01-tracer.md
git commit -m "feat(tracer): .ARM.exidx 精确回溯 + 工具链守卫 + 帧指针链；补充 WORKLOG 业界方案全景"
```

---

# 追加：离线解析（crash dump → 符号）已实现（次日完善会话）

## 背景
前面"未实现方案"中最有价值的是**离线解析**——把 fault dump 的 PC + 原始栈转成可读调用链，
且对无 exidx 的 IAR / ARMCC5 是获得精确回溯的唯一可行路径。今天落地。

## 实现
1. **tracer.c fault handler 新增 "Raw stack" hex dump**：
   - 新宏 `TRACER_STACK_DUMP_BYTES`（tracer.h，默认 256，0=关闭）；
   - 从 `f.sp` 起 dump N 字节，截断到 `tracer_stack_limit()`/栈顶（防越界）；
   - 格式：`Raw stack (0xADDR, N bytes):` + 逐行 `  ADDR: xx xx ...`。
2. **顺带修复扫描法 RTOS clamp 隐患**（前面"顺便暴露"那条）：
   - 删掉 `tracer_walk_callstack` 的 `if (scan < TRACER_STACK_BASE) scan = TRACER_STACK_BASE;`；
   - 原因：RTOS 任务栈在 ucHeap（0x8020xxxx）**远低于主栈 `_sstack=0x80ffeff0`**，
     clamp 会把 scan 抬到主栈底 → 任务栈 fault 扫描 0 帧；scan 总来自真实 SP/异常帧，无需 clamp。
3. **新工具 `works/tools/tracer_decode.py`**（依赖 pyelftools：`pip install pyelftools`）：
   - 解析 dump 日志：`text [..]` 范围 / 寄存器块 / `Call stack:` / `Raw stack`；
   - pyelftools 读 ELF `.symtab` 构建函数区间表，二分查找 PC → `函数名+偏移`；
   - Raw stack 按 **4 字节组合 little-endian**，筛出 `.text` 内的 Thumb 返回地址候选并符号化；
   - 用法：
     ```
     python tracer_decode.py <elf> <dump.log>   # 解析日志
     python tracer_decode.py <elf> -            # 从 stdin
     python tracer_decode.py <elf> <pc> ...     # 裸地址快速符号化
     ```

## 验证（QEMU mps2-an505，临时 `test5()` 触发 UsageFault）
```
PC  =100BFB3A  tracer_trigger_unalign+0x11
LR  =1000196D  main_task_entry+0x8
Call stack: 100BFAF4 tracer_dump_callstack / 1000193E test5 / 1000196C main_task_entry
Raw stack return-address candidates:
  [802150A8] 10001964  test5+0x5B        ← 扫描法遗漏的调用链证据
```
验证后移除 test5 触发；`build-phone` / `build-baremetal` 回归干净（boot 正常，无 fault）。

## 脚本开发踩到的三个小坑
1. **参数判断**：日志文件被误判为裸地址 → 改为"单个参数且是 `-` 或存在的文件 = 日志，否则全部当地址"。
2. **Raw stack 正则**：`\s*` 贪婪吞换行 + `{47}` 长度错位导致 group 空 → 改逐行扫描解析。
3. **字节 vs 字**：Raw stack 必须按 4 字节组合成 little-endian uint32 才能判返回地址（按单字节会全漏）。

## 业界方案状态更新
| 方案 | 状态 |
|---|---|
| 栈扫描（BL/BLX） | ✅ 兜底 |
| `.ARM.exidx` 展开表 | ✅ GCC/armclang |
| 帧指针链 | ⚠️ Cortex-M 受限 |
| **离线解析 crash dump → 符号** | ✅ **今天已实现**（`tracer_decode.py`） |
| 影子栈 | 未实现（需编译器/硬件，非本库范畴） |
| `-finstrument-functions` 动态轨迹 | 未实现（目标不同，需要时再做） |

## 本次变更文件
```
M libutils/tracer/tracer.c     # Raw stack hex dump + 扫描 clamp 修复
M libutils/tracer/tracer.h     # TRACER_STACK_DUMP_BYTES 宏
A works/tools/tracer_decode.py # 离线符号化工具（pyelftools）
M .gitignore                   # 忽略 __pycache__/ *.pyc
```

建议提交：
```bash
git add libutils/tracer/tracer.c libutils/tracer/tracer.h works/tools/tracer_decode.py .gitignore works/logs/WORKLOG-2026-09-01-tracer.md
git commit -m "feat(tracer): 离线解析 crash dump→符号（raw stack dump + tracer_decode.py）；修复扫描法 RTOS clamp"
```
