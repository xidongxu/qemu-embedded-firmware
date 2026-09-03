# tracer — Cortex-M 故障转储（Fault Dump）库

一个极简、自包含的 Cortex-M（M3..M85）异常/fault 诊断库：当单片机发生 HardFault /
BusFault / UsageFault / MemManage / NMI / SecureFault 时，打印完整的现场（寄存器、
异常状态、调用栈、原始栈、崩溃前函数轨迹），配合离线解析工具把地址还原成可读的函数
调用链，快速定位崩溃点。

- **零依赖**：不依赖 CMSIS / RTOS；寄存器全部裸地址访问。输出默认走 printf，也可定义
  `TRACER_PUTCHAR` 走库内无锁 mini-printf（此时连 printf 都不链接，fault 路径锁安全）。
- **单目录**：所有文件放在本目录，便于并入任意工程。
- **多工具链**：GCC / armclang / IAR / ARMCC 均可编译（按工具链选对应汇编入口）。
- **多内核**：M3 / M4 / M7 / M23 / M33 / M55 / M85（**M0/M0+ 不支持**：Thumb-1 无多寄存器 `PUSH`，
  向量入口无法保存 r4..r11）。

---

## 一、目录文件

| 文件 | 说明 |
|---|---|
| `tracer.h` | 公共头文件：配置宏、数据结构、API、weak hooks |
| `tracer.c` | 核心逻辑：寄存器/异常解码、FPU 上下文、调用栈、原始栈、轨迹、断言、自动复位、trap |
| `tracer_gnugcc.s` | 汇编入口（**GCC / Clang / armclang / ARMCC6** 用） |
| `tracer_iccarm.s` | 汇编入口（**IAR EWARM** 用） |
| `tracer_armcc.s` | 汇编入口（**MDK armasm / ARMCC5** 用） |
| `CMakeLists.txt` | CMake 构建（自动按编译器选汇编入口 + 工具链守卫 + `tracer::tracer` alias） |
| `tracer_parser.py` | 离线解析工具：dump 日志 + ELF → 符号化调用链，并支持 `.ARM.exidx` 离线逐帧展开、FPU 寄存器转浮点 |
| `tests/` | host 单测（`test_tracer.c` 纯逻辑 + `test_parser.py` 离线解析），`-DTRACER_BUILD_TESTS=ON` 启用 |
| `LICENSE` | MIT 许可证 |
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
FW     : v1.2.3                     ← 固件版本（TRACER_FW_VERSION）
Up     : 123456 ms                  ← 系统已运行时长（weak hook tracer_uptime_ms）
Exception : UsageFault (IPSR=0)
EXC_RETURN: 0xFFFFFFFD  [Thread mode, PSP, Secure]
 R0..R11 / R12 SP LR PC / xPSR
 FPU (extended frame): S0..S15 / FPSCR   ← 可选（FPU 活动时自动解码）
 CFSR=01000000  MMFSR=00  BFSR=00  UFSR=0100
 HFSR=40000000  DFSR=00000000
 MMFAR=00000000  BFAR=00000000
 Call stack: 100020C8 10002048 ...
 Function trace (last 128):        ← 可选（TRACER_USE_FINSTRUMENT）
 Raw stack (0x80215080, 48 bytes): ← 可选（TRACER_STACK_DUMP_BYTES）
===== End of dump =====
```

> dump 全程**关中断**（汇编入口 `cpsid i`）+ **重入保护**（`s_tracer_dumping`）：
> 扫描/打印期间即使再触发 fault（如 NMI，无法被 `cpsid i` 屏蔽）也只打印 note，绝不递归。
>
> **crash-safe 输出**：定义 `TRACER_PUTCHAR(ch)`（如裸 UART 数据寄存器写 / Segger RTT）后，
> 整个 dump 由库内**无锁 mini-printf** 逐字符渲染，完全不碰 stdio/printf——即使 fault 打断的是
> printf 本身或它持有的锁也不会死锁（QEMU 实测输出与 printf 模式逐字一致）。未定义时回退
> TRACER_PRINTF（默认 printf，简单但 fault 恰在 printf 内时可能死锁）。

### 2. 主动调用 API
| API | 作用 |
|---|---|
| `tracer_init()` | 开机调用一次，打印版本 + text/stack 范围（对 addr2line 定位极有用） |
| `tracer_dump_callstack()` | 主动打印当前调用栈（从调试命令调用） |
| `tracer_get_callstack(buf, size)` | 把当前调用栈 PC 写入缓冲区（供落盘/协议发送） |
| `tracer_dump_all()` | 主动打印完整快照（版本 + 任务 + 调用栈 + 轨迹），挂 shell/调试命令 |
| `tracer_trigger_unalign()` | 主动触发 UsageFault（测试 fault 路径用） |
| `TRACER_ASSERT(expr)` | 断言：失败打印表达式 + file:line + 调用栈，然后自动复位/或 trap（替代裸 assert） |
| `tracer_assert_fail(...)` | `TRACER_ASSERT` 的底层实现（一般不直接调） |

### 3. 可覆盖的 weak hooks
| Hook | 默认 | 用途 |
|---|---|---|
| `tracer_on_fault(&f)` | 空 | fault 时先调用，可打印当前 FreeRTOS 任务名；`&f` 含 FPU 上下文指针 `fpu`（S0..S15+FPSCR） |
| `tracer_stack_limit()` | 主栈顶 | 返回扫描上界；FreeRTOS 下返回当前任务栈顶 `pxEndOfStack` |
| `tracer_dump_tasks()` | 空 | fault 时列出所有任务（状态/栈水位）；RTOS 适配（FreeRTOS: `vTaskList`），核心保持 RTOS 无关 |
| `tracer_uptime_ms()` | 0 | 系统运行毫秒数，每个 dump 打印 `Up:`；FreeRTOS 覆盖成 `xTaskGetTickCount()*portTICK_PERIOD_MS` |
| `tracer_watchdog_kick()` | 空 | 喂独立看门狗；`TRACER_AUTO_RESET_MS` 延时期间周期性调用，防止 dump/延时被狗截断（纯 trap 模式不喂，狗作最后兜底） |

### 3.5 FPU/MVE 上下文解码（新增）
当 faulting 上下文使用了 FPU/MVE（`EXC_RETURN` bit4=0 = 扩展帧）时，dump 自动打印 `S0..S15 + FPSCR`
（16 进制；离线解析脚本会转成浮点）。**实现要点（QEMU 实测）**：
- 硬件只在**基本帧上方**预留 72 字节扩展区；在 **lazy stacking**（`FPCCR.LSPEN=1`，RTOS 默认）下，
  S 寄存器值要等 handler 第一次碰 FPU 才真正写入——所以 tracer 用一条 `vmov s0,s0` 触发 lazy save，
  再从 **FPCAR**（`0xE000EF38`）读取；eager（LSPEN=0）时回退读栈上扩展区。
- 需要工具链 FPU ISA 开启（GCC `-mfpu` / IAR），否则 `fpu=NULL`。`S16..S31` 为调用者保存
  寄存器，异常入口后在 lazy stacking 下处理器不再保留 faulting 上下文的值（QEMU M33 实测读 0），
  故不采集。

### 4. 多种调用栈回溯方式（按需/按工具链）
| 方式 | 开关 | 适用 | 说明 |
|---|---|---|---|
| **栈扫描**（BL/BLX） | 默认兜底 | 所有工具链 | 零依赖；-O2 高度优化下可能 0 帧 |
| **.ARM.exidx 精确回溯** | `TRACER_USE_EXIDX=1` | GCC/armclang | 需 `-funwind-tables`；-O2 下精确，**推荐** |
| **帧指针链** | `TRACER_USE_FP=1` | A32/IAR | Cortex-M(Thumb) 上仅最内层帧，**不推荐** |
| **动态函数轨迹** | `TRACER_USE_FINSTRUMENT=1` | GCC/armclang | 崩溃前函数进出回放（`+delta` = SysTick 周期，相对耗时），需 `-finstrument-functions` |
| **离线解析** | 无（事后用脚本） | 所有工具链 | `tracer_parser.py` 把 dump + ELF → 符号化调用链；带 PC+SP+原始栈时还会用 `.ARM.exidx` 在**主机上逐帧展开**出精确调用链 |

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

**方式 A2：install 后用 `find_package`（外部独立工程）**
```
cmake -S libutils/tracer -B build-tracer \
      -DCMAKE_TOOLCHAIN_FILE=<...>/arm-none-eabi-gcc.cmake \
      -DTRACER_EXIDX_TABLES=ON   # 可选，见“编译选项”
cmake --build build-tracer
cmake --install build-tracer --prefix <prefix>
```
消费工程（装好 tracer 后）：
```cmake
find_package(tracer CONFIG REQUIRED)
target_link_libraries(<app> PRIVATE tracer::tracer)
# 仍需 whole-archive 语义：tracer::tracer 的 archive 必须整体拉入才能覆盖 startup weak，
# 链接时用 --whole-archive 包住（CMake 工程见方式 A 的写法，或手动
# -Wl,--whole-archive -ltracer -Wl,--no-whole-archive）。
```
头文件装到 `<prefix>/include/tracer/tracer.h`；tracer 库本身会编 `-std=c99`。

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
- **exidx 精确回溯**：需要 tracer 库自身带 `.ARM.exidx` 时开 option
  `-DTRACER_EXIDX_TABLES=ON`（默认 OFF，省 flash；add_subdirectory 方式下因 boards 后于
  libutils 加载无法用 option，可直接对 tracer 目标补
  `target_compile_options(tracer PRIVATE -funwind-tables)`），再
  `target_compile_definitions(tracer PRIVATE TRACER_USE_EXIDX=1)`（**必须定义在 tracer 库目标**，
  定义在 app 目标不生效）；整个 app 的源也建议 `-funwind-tables` 让回溯链覆盖 app 帧。
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
| `TRACER_PRINTF` | `printf` | 常规输出函数（可重定向） |
| `TRACER_PUTCHAR` | 无 | **crash-safe** 逐字符输出（裸 UART/RTT）；定义后 dump 走无锁 mini-printf，不碰 printf |
| `TRACER_STACK_DEPTH` | 32 | 调用栈条目上限 |
| `TRACER_STACK_DUMP_BYTES` | 256 | 原始栈 hex 字节数，0=关闭 |
| `TRACER_USE_EXIDX` | 0 | 启用 .ARM.exidx 精确回溯 |
| `TRACER_USE_FINSTRUMENT` | 0 | 启用动态函数轨迹 |
| `TRACER_TRACE_DEPTH` | 128 | 轨迹环形缓冲大小 |
| `TRACER_USE_FP` | 0 | 帧指针链（Cortex-M 不推荐） |
| `TRACER_FW_VERSION` | `"0.0.0"` | 固件版本字符串，`tracer_init`/dump 头部打印（现场对版用） |
| `TRACER_AUTO_RESET_MS` | 0 | >0 时 dump 后延时该毫秒数自动系统复位（0=永久 trap） |
| `TRACER_ASSERT(expr)` | 见宏 | 断言宏（打印 + 调用栈 + 自动复位/trap） |
| `TRACER_USE_CRASH` | 0 | 崩溃黑匣子（见下）：预崩溃环形日志 + dump 镜像捕获 + weak 持久化钩子 |
| `TRACER_USE_LOG` | 0 | 分级运行日志（见下）：`tracer_log`/`TRACER_LOGI..` + sink/drain 异步接口 |
| `TRACER_RING_SIZE` | 2048 | 共享环形缓冲（`tracer_ring_printf()` 事件与 `tracer_log()` 日志共用，字节） |
| `TRACER_CRASH_SIZE` | 8192 | 崩溃 record 捕获缓冲（dump 文本 + ring 尾 + CRC footer） |
| `TRACER_LOG_DEFAULT_LEVEL` | `INFO` | `tracer_log` 运行期初始级别（可随时 `tracer_log_set_level` 调） |
| `TRACER_LOG_LINE_SIZE` | 160 | `tracer_log` 单行上限（前缀+内容+CRLF，超长截断） |

### 崩溃黑匣子（crash black box，可选，TRACER_USE_CRASH=1）

把"宕机现场 + 宕机前发生了什么"自动留下来，重启后读回——工厂/现场无人值守诊断用。

- **预崩溃日志**：应用在关键事件处调 `tracer_ring_printf("...")`（无锁、IRQ 安全），tracer 在 RAM 保留最近
  `TRACER_RING_SIZE` 字节（"黑匣子"）。
- **崩溃捕获**：fault/assert/栈溢出 dump 时，每个输出字符同时镜像进 RAM 捕获缓冲（因此 TRACER_USE_CRASH 会
  强制 dump 走逐字 mini-printf——无 `TRACER_PUTCHAR` 时回退 `putchar()`，有则 crash-safe）；收尾时把 ring 尾
  与 CRC-32 footer 附加成一条完整 record 文本，交给 weak `tracer_crash_save(data,len)`（默认 no-op）。
- **两段式持久化**（设计要点，也是与"崩溃时直接开文件系统写"的区别）：
  ① 现场（系统不可信）只做**防御性裸写**到保留存储（固定扇区/槽 + CRC，极小代码路径，不依赖文件系统——FS
  的状态/锁/栈在崩溃现场可能已坏）；② 重启后（系统可信）再由 boot 代码把 record 读回并**归档成文件/上报**。
- **存储（两层，介质无关策略 + 板级介质）**：
  - 策略层 `tracer_crash_store.c`（编译进 tracer，随 `TRACER_USE_CRASH`）：提供 `tracer_crash_save()`
    的**通用实现**——保留区划成 N 槽（默认 2，槽=一次擦除单位），双槽交替写、槽头 `'TNC1'|len|crc32`、
    断电半写不毁旧记录、`tracer_crash_store_read_latest()`/`tracer_crash_store_clear()`。介质差异全部收进
    4 个 weak 原语 `tracer_crash_store_get_media/erase/write/read`（无介质即静默 no-op，无需板胶水）。
  - 板级：只实现这 4 个介质原语 + 各自的 boot 归档。mps2-an505 参考 `crash_nv.c`：SPI NOR 顶部保留
    2×4K、littlefs `crash_last.txt`/`crash_prev.txt` 归档并清除 staging（下次 boot 不重复上报）。换板
    （如 stm32 内部 Flash）只需重写介质原语，双槽/防半写/校验逻辑直接复用。
  - QEMU 验证需带 `w25q02jvm` 的补丁版 QEMU + `-drive if=mtd`。
- 内容为**纯文本**：人可读、`tracer_parser.py` 可直接符号化、跨平台无私有二进制 ABI。

### 分级运行日志（leveled runtime log，可选，TRACER_USE_LOG=1）

`TRACER_USE_CRASH` 与 `TRACER_USE_LOG` 是两个**互相独立**的开关，可单独开：

- **只开 `TRACER_USE_CRASH`**：预崩溃事件 + 崩溃 record 落存储，无 `tracer_log`。
- **只开 `TRACER_USE_LOG`**：`tracer_log()` 同步串口打印 + sink/drain 异步接口（ring 作为 drain 缓冲池），
  无崩溃捕获/存储。
- **两者都开**：`tracer_log()` 与 `tracer_ring_printf()` 写**同一 ring**（合一）——运行时出问题后崩溃
  record 自动含宕机前运行日志，无需刻意再调一次存储接口。

把 `tracer_log()` 当成普通日志 API 用即可：应用只要链上 tracer 并开了 `TRACER_USE_LOG`，调用它就能打印。

- **API**：`uint32_t tracer_log(tracer_log_level_t level, const char *fmt, ...)`。
  级别：`TRACER_LOG_TRACE/DEBUG/INFO/WARN/ERROR`（0..4）。行格式：`[<ms> ms]X: <内容>\r\n`，`X` 为
  `T/D/I/W/E`。`<ms>` 来自 weak `tracer_uptime_ms()`——**默认已带 SysTick wrap 计数**（SysTick 以 ~1ms
  reload 跑时=真实运行毫秒，免接线），RTOS/应用可覆盖成更准的 tick（FreeRTOS 用
  `xTaskGetTickCount()*portTICK_PERIOD_MS`，mps2 已覆盖）。
- **分级便捷宏（不带 level 参数）**：级别写死在宏名里，调用不再传 level——`TRACER_LOGI("call %u", n)` ≡
  `tracer_log(TRACER_LOG_INFO, ...)`。一组 `TRACER_LOGT/LOGD/LOGI/LOGW/LOGE`；仍受运行期级别过滤。
- **运行期分级开关**（非编译期过滤）：所有级别都编译进去，输出与否看运行值。默认 `TRACER_LOG_DEFAULT_LEVEL`
  = `INFO`（可 `-D` 覆盖）；任意时刻用 `tracer_log_set_level()`/`tracer_log_get_level()` 调整（如加一条 shell
  命令 `log level`，调试时提到 TRACE、正式跑提到 WARN）。
- **每行输出三步（一次临界区内）**：① 同步打到串口（不对接任何异步后端时，这就是"同步日志"）；② 写入
  共享崩溃 ring（与 `tracer_ring_printf()` 同一缓冲）；③ 出临界区后把整行 `(line,len)` 交给 weak
  `tracer_log_sink()`。行缓冲在**调用者栈上**（`TRACER_LOG_LINE_SIZE`，默认 160，超长截断仍保留 `\r\n`），
  因此 `tracer_log()` 可重入、可在 ISR 里调用。
- **预留异步存储接口（写文件 / 写 flash），两选一或都用；都不实现则纯同步输出**：
  - **push（weak 回调）**：覆盖 `void tracer_log_sink(const void *line, uint32_t len)`（默认 no-op），每行
    完成后回调给你，自行追加到文件/flash。例如接到 littlefs：
    ```c
    void tracer_log_sink(const void *line, uint32_t len) {
        /* 追加到日志文件（非崩溃路径，可用 FS/锁） */
        log_file_append(line, len);
    }
    ```
  - **pull（增量导出）**：后台低优先级任务/空闲钩子调
    `uint32_t tracer_log_drain(uint8_t *out, uint32_t max)`，拿到自上次以来的新增字节流（内含事件与日志的
    混合顺序），自行落盘；ring 被消费过慢覆盖时，从仍可用的最旧字节开始返回。
- 行缓冲宏：`TRACER_LOG_LINE_SIZE`（默认 `160u`，单行上限，可 `-D` 覆盖）。
- host 单测 `tests/test_tracer_log.c`（格式/运行期分级/合一 ring/截断/drain/崩溃 record 自动含最近日志）与
  `tests/test_tracer_log_sink.c`（weak sink 强覆盖、过滤不回调）随 CI 运行。

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

> 若检测到 `arm-none-eabi-addr2line`（或设 `TRACER_ADDR2LINE` 环境变量指向它），
> 输出会**附加源码行号** `(file:line)`（-O2 下部分地址无精确行号会显示 `?`）。
> 建议 ELF 保留 `-g` 调试信息以获得完整行号。
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

Exidx offline unwind (exact, from PC + raw stack):  ← 离线 .ARM.exidx 展开
  100BFB3A  tracer_trigger_unalign  ->  1000196C  main_task_entry
  1000196C  main_task_entry        ->  10006224  pj_test_run
```

> `.ARM.exidx` 离线展开说明：`tracer_parser.py` 读 ELF 的 `.ARM.exidx/.ARM.extab`
> （EHABI），从 dump 的 PC/SP/LR + Raw stack 出发在**主机上逐帧展开**，无启发式误报，
> 比扫描法精确（能体现 -O2 内联后的真实调用关系）。展开深度受 `TRACER_STACK_DUMP_BYTES`
> 限制——原始栈越大展开越深。这是无 exidx 运行时（IAR/ARMCC5）获得精确回溯的路径。

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
