# tracer — Cortex-M 故障转储（Fault Dump）库

一个极简、自包含的 Cortex-M（M3..M85）异常/fault 诊断库：当单片机发生 HardFault /
BusFault / UsageFault / MemManage / NMI / SecureFault 时，打印完整的现场（寄存器、
异常状态、调用栈、原始栈、崩溃前函数轨迹），配合离线解析工具把地址还原成可读的函数
调用链，快速定位崩溃点。

- **零依赖**：不依赖 CMSIS / RTOS / printf（寄存器全部用裸地址访问）。
- **单目录**：所有文件放在本目录，便于并入任意工程。
- **多工具链**：GCC / armclang / IAR / ARMCC 均可编译（按工具链选对应汇编入口）。
- **多内核**：M3 / M4 / M7 / M23 / M33 / M55 / M85（M0/M0+ 需把汇编里的 `push {r4-r11}` 拆成两次）。

---

## 一、目录文件

| 文件 | 说明 |
|---|---|
| `tracer.h` | 公共头文件：配置宏、数据结构、API、weak hooks |
| `tracer.c` | 核心逻辑：寄存器/异常解码、调用栈、原始栈、轨迹、trap |
| `tracer_gnugcc.s` | 汇编入口（**GCC / Clang / armclang / ARMCC6** 用） |
| `tracer_iccarm.s` | 汇编入口（**IAR EWARM** 用） |
| `tracer_armcc.s` | 汇编入口（**MDK armasm / ARMCC5** 用） |
| `CMakeLists.txt` | CMake 构建（自动按编译器选汇编入口 + 工具链守卫） |
| `tracer_parser.py` | 离线解析工具（把 dump 日志 + ELF → 符号化调用链） |
| `README.md` | 本文档 |

> `works/tools/tracer_decode.py` 是 `tracer_parser.py` 的仓库级副本（保持同步）。

---

## 二、这个库能干什么（功能清单）

### 1. 自动接管异常向量（fault 时自动 dump，无需改 startup）
导出**强符号** `HardFault_Handler` / `MemManage_Handler` / `BusFault_Handler` /
`UsageFault_Handler` / `NMI_Handler` / `SecureFault_Handler`，覆盖 startup 里的 weak 默认处理。
fault 一发生就打印：

```
===== Tracer: UsageFault Fault Dump =====
Exception : UsageFault (IPSR=0)
EXC_RETURN: 0xFFFFFFFD  [Thread mode, PSP, Secure]
 R0..R11 / R12 SP LR PC / xPSR
 CFSR=01000000  MMFSR=00  BFSR=00  UFSR=0100
 HFSR=40000000  DFSR=00000000
 MMFAR=00000000  BFAR=00000000
 Call stack: 100020C8 10002048 ...
 Function trace (last 128):        ← 可选（TRACER_USE_FINSTRUMENT）
 Raw stack (0x80215080, 48 bytes): ← 可选（TRACER_STACK_DUMP_BYTES）
===== End of dump =====
```

### 2. 主动调用 API
| API | 作用 |
|---|---|
| `tracer_init()` | 开机调用一次，打印 text/stack 范围（对 addr2line 定位极有用） |
| `tracer_dump_callstack()` | 主动打印当前调用栈（从调试命令调用） |
| `tracer_get_callstack(buf, size)` | 把当前调用栈 PC 写入缓冲区（供落盘/协议发送） |
| `tracer_trigger_unalign()` | 主动触发 UsageFault（测试 fault 路径用） |

### 3. 可覆盖的 weak hooks
| Hook | 默认 | 用途 |
|---|---|---|
| `tracer_on_fault(&f)` | 空 | fault 时先调用，可打印当前 FreeRTOS 任务名 |
| `tracer_stack_limit()` | 主栈顶 | 返回扫描上界；FreeRTOS 下返回当前任务栈顶 `pxEndOfStack` |

### 4. 多种调用栈回溯方式（按需/按工具链）
| 方式 | 开关 | 适用 | 说明 |
|---|---|---|---|
| **栈扫描**（BL/BLX） | 默认兜底 | 所有工具链 | 零依赖；-O2 高度优化下可能 0 帧 |
| **.ARM.exidx 精确回溯** | `TRACER_USE_EXIDX=1` | GCC/armclang | 需 `-funwind-tables`；-O2 下精确，**推荐** |
| **帧指针链** | `TRACER_USE_FP=1` | A32/IAR | Cortex-M(Thumb) 上仅最内层帧，**不推荐** |
| **动态函数轨迹** | `TRACER_USE_FINSTRUMENT=1` | GCC/armclang | 崩溃前函数进出回放，需 `-finstrument-functions` |
| **离线解析** | 无（事后用脚本） | 所有工具链 | `tracer_parser.py` 把 dump + ELF → 符号化调用链 |

优先级：`exidx > FP > 扫描`（主动 dump 用）；fault handler 路径固定用扫描；
动态轨迹 / 原始栈 dump 是与回溯**正交**的增强，可同时开启。

### 5. 原始栈 + 离线符号解析（无 exidx 工具链也能精确回溯）
fault 时把 fault 现场附近的原始栈 hex 打出来，事后用 `tracer_parser.py` + ELF 还原调用链。

---

## 三、什么场景下使用

- **开发调试期定位崩溃**：跑起来后随机/偶发 HardFault，用它打印现场 + 调用链 + 崩溃前轨迹。
- **RTOS 任务崩溃**：能显示 `CurrentTask: <任务名>` + 该任务栈的调用链（不会跨栈误报）。
- **网络/协议栈异常**：lwIP、SIP(pjproject) 解析外部数据崩溃时，用轨迹回放看"怎么走进来的"。
- **现场问题复现**：把 dump 日志 + ELF 发给分析，`tracer_parser.py` 离线还原，无需目标机。
- **没有调试器/断点不便**的场景（QEMU、量产板），fault dump 是主要诊断手段。

---

## 四、怎样集成到单片机工程

### 方式 A：CMake（推荐）
```cmake
add_subdirectory(<path>/tracer)
# 强符号接管 startup，必须 whole-archive，否则静态库不被拉入：
target_link_libraries(<app> PRIVATE
    -Wl,--whole-archive tracer -Wl,--no-whole-archive
)
target_include_directories(<app> PRIVATE ${CMAKE_SOURCE_DIR}/libutils/tracer)
```
`CMakeLists.txt` 已按 `CMAKE_C_COMPILER_ID` 自动选汇编入口（GNU/Clang→`tracer_gnugcc.s`，
IAR→`tracer_iccarm.s`，ARMCC→`tracer_armcc.s`），不支持的工具链会 `FATAL_ERROR` 提示。

### 方式 B：Makefile（stm32 CubeMX 风格）
- C 源：加入 `libutils/tracer/tracer.c`；
- 汇编源：按工具链选 **一个** 加入（勿混用）：
  - gcc 分支 → `tracer_gnugcc.s`
  - iar 分支 → `tracer_iccarm.s`
  - mdk 分支 → `tracer_armcc.s`
- include 路径加 `-I<path>/libutils/tracer`；
- 若用 CubeMX 生成的 `stm32f4xx_it.c`，把其中 `NMI/MemManage/BusFault/UsageFault_Handler`
  声明为 `TRACER_WEAK`（`__attribute__((weak))`），否则强符号冲突。

### 方式 C：IAR / MDK IDE
- 加入 `tracer.c` + 对应汇编文件（IAR 用 `tracer_iccarm.s`，MDK 用 `tracer_armcc.s`）；
- include `tracer.h` 路径；
- 项目编译选项见下文"条件"。

### 使用步骤（三行接入）
```c
#include "tracer.h"

int main(void) {
    tracer_init();          // 1. 开机初始化（打印 text/stack 范围）
    /* 可选：FreeRTOS 下覆盖 hooks，打印任务名 + 限任务栈顶 */
    ...
}
```
之后任何 fault 都会自动 dump。fault 后 `tracer_fault_handler` 会 `for(;;)` trap 住。

---

## 五、使用条件

### 工具链
| 工具链 | 汇编入口 | exidx | finstrument |
|---|---|---|---|
| GCC / armclang / ARMCC6 | `tracer_gnugcc.s` | ✅ | ✅ |
| IAR EWARM | `tracer_iccarm.s` | ❌（回退扫描） | ⚠️（需编译器支持） |
| MDK ARMCC5 | `tracer_armcc.s` | ❌（回退扫描） | ⚠️ |

### 编译选项（按需）
- **exidx 精确回溯**：整个工程（含 tracer 库自身）加 `-funwind-tables`，并 `target_compile_definitions(tracer PRIVATE TRACER_USE_EXIDX=1)`（**必须定义在 tracer 库目标**，定义在 app 目标不生效）。
- **动态轨迹**：`-finstrument-functions` 只加到要跟踪的源文件（`set_source_files_properties`），并 `target_compile_definitions(tracer PRIVATE TRACER_USE_FINSTRUMENT=1)`。
- **栈扫描 / 离线解析**：无额外选项，默认即可。

### 链接
- **必须 whole-archive**（静态库"引用才拉入"，而向量表引用已被 startup 的 weak 满足）：
  `-Wl,--whole-archive tracer -Wl,--no-whole-archive`。
- ARMCC5 需 `--c99`（否则 `inline` 未定义）。

### 内存映射符号
扫描/回溯默认取链接器符号（GNU `_stext/_etext/_sstack/_estack`、ARMCC `Image$$...`、
IAR `__section_begin/end`）。链接脚本没有这些符号时，用 `-D` 覆盖：
```
-DTRACER_TEXT_START=0x08000000 -DTRACER_TEXT_END=0x08100000
-DTRACER_STACK_BASE=0x20000000 -DTRACER_STACK_TOP=0x20020000
```

### 配置宏一览（tracer.h，均可 `-D` 覆盖）
| 宏 | 默认 | 说明 |
|---|---|---|
| `TRACER_PRINTF` | `printf` | 输出函数（可重定向） |
| `TRACER_STACK_DEPTH` | 32 | 调用栈条目上限 |
| `TRACER_STACK_DUMP_BYTES` | 256 | 原始栈 hex 字节数，0=关闭 |
| `TRACER_USE_EXIDX` | 0 | 启用 .ARM.exidx 精确回溯 |
| `TRACER_USE_FINSTRUMENT` | 0 | 启用动态函数轨迹 |
| `TRACER_TRACE_DEPTH` | 128 | 轨迹环形缓冲大小 |
| `TRACER_USE_FP` | 0 | 帧指针链（Cortex-M 不推荐） |

---

## 六、产生 dump 后：怎么解析、怎么定位问题

### 1. 先把 dump 从串口/日志存成文件（如 `dump.log`）

### 2. 用离线解析工具还原符号
```bash
python tracer_parser.py <你的.elf> dump.log
# 或从 stdin
cat dump.log | python tracer_parser.py <你的.elf> -
# 或直接符号化几个地址
python tracer_parser.py <你的.elf> 0x1000196D 0x100BFB3A
```
输出示例：
```
=== tracer fault dump decode ===
ELF: an505-qemu.elf
text [10000000 - 101CD324]

PC  =100BFB3A  tracer_trigger_unalign+0x11     ← 崩溃指令所在函数
LR  =1000196D  main_task_entry+0x8             ← 调用方
SP  =80215268

Call stack (from tracer):
  100C250C  tracer_dump_callstack+0x17
  10001FCC  test0+0x1F
  ...

Function trace (last 15):                      ← 崩溃前执行路径
  -> main_task_init → dump_callstack → test5 → test4 → ... → test0 → main

Raw stack return-address candidates:           ← 栈上残留的返回地址
  [80215268] 100019A0  tracer_stack_limit+0x3F
```

### 3. 定位流程（按优先级）
1. **看 PC**：`tracer_parser.py` 输出的 `PC =xxx func+0x偏移` 就是崩溃指令所在函数；
   用 `addr2line -e <elf> -f -C <PC>` 可进一步拿到**源码行号**。
2. **看异常类型 + 状态寄存器**：
   - `CFSR/MMFSR/BFSR/UFSR` 告诉你是什么 fault（越界读/写、未对齐、指令异常…）；
   - `BFAR/MMFAR [VALID]` 给出出错地址（如 `0xDEADBEEF` 多半是野指针）；
   - `HFSR=0x40000000 FORCED` 表示低级 fault 被升级成 HardFault，根因看 CFSR。
3. **看 Call stack**：调用链，从内到外定位是谁调的。
4. **看 Function trace**：崩溃前最后执行了哪些函数（"怎么走到这的"），
   尤其适合偶发/时序类问题。
5. **看 Raw stack**：栈上残留的返回地址候选，补充调用链（扫描法漏掉的帧）。
6. **结合 HMP/调试器**：对照 `SP` 地址看内存。

> 快速命令：
> ```
> arm-none-eabi-addr2line -e an505-qemu.elf -f -C 0x100BFB3A 0x1000196D
> ```

---

## 七、常见问题 / 坑（都是实测踩过的）

1. **`TRACER_USE_EXIDX` / `TRACER_USE_FINSTRUMENT` 必须定义在 tracer 库目标**，
   定义在 app 目标不传到单独编译的 tracer.c（宏/回调不生效）。
2. **exidx 还要 tracer 库自身带 `-funwind-tables`**，否则从库内帧回溯直接 0 帧。
3. **帧指针链在 Cortex-M 上只有最内层 1 帧**（GCC Thumb 用 r7 非标准帧布局 +
   `__builtin_return_address` 深度受限），请用 exidx 或离线解析。
4. **`-finstrument-functions` 会增大栈占用**，紧凑任务栈可能触发 FreeRTOS 溢出检测；
   且全量开会记录 printf 实现（如 LVGL 的 `lv_sprintf_builtin`），轨迹被刷屏，
   **务必按文件开启**。
5. **RTOS 任务栈通常在主栈（MSP）之下**，扫描/回溯请用 `tracer_stack_limit()`
   返回任务栈顶，否则跨栈误报。
6. **线程模式 fault 时 xPSR.IPSR=0 是正常的**，别误判；异常类型看 CFSR。
7. **EXC_RETURN bit2=0x4 才是 PSP**（不是 bit1）。
