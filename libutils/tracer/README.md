# tracer

**Cortex-M（M3..M85）单片机故障诊断库** —— fault / assert 现场转储、调用栈回溯、
崩溃黑匣子（可选）、分级运行日志（可选）。自包含、零依赖（无 CMSIS / RTOS / printf
依赖），GCC / armclang / IAR / ARMCC 多工具链，配每核一块 QEMU board 的全架构验证矩阵。

[![CI](https://github.com/xidongxu/tracer/actions/workflows/ci.yml/badge.svg)](https://github.com/xidongxu/tracer/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Cortex-M3..M85](https://img.shields.io/badge/Cortex--M-M3%20M4%20M7%20M23%20M33%20M55%20M85-informational)

---

## 目录

- [特性](#特性)
- [目录结构](#目录结构)
- [支持范围](#支持范围)
- [快速上手](#快速上手)
- [功能详解](#功能详解)
- [配置参考](#配置参考)
- [解析 dump 与定位问题](#解析-dump-与定位问题)
- [测试与覆盖率](#测试与覆盖率)
- [CI 与发布](#ci-与发布)
- [常见问题 FAQ](#常见问题-faq)
- [许可与出处](#许可与出处)

---

## 特性

- **自动接管 fault 向量**：导出强符号 `HardFault_Handler` / `MemManage_Handler` /
  `BusFault_Handler` / `UsageFault_Handler` / `NMI_Handler` / `SecureFault_Handler`，
  覆盖 startup 的 weak 默认 —— fault 一发生即自动 dump，无需改 startup。
- **完整现场**：寄存器 + 异常状态（`CFSR/MMFSR/BFSR/UFSR/HFSR` + `BFAR/MMFAR`）+
  调用栈 + 原始栈 hex + 崩溃前函数轨迹 + FPU/MVE 扩展帧。
- **crash-safe 输出**：可选 `TRACER_PUTCHAR` 走库内无锁 mini-printf；即使 fault 打断
  的是 printf 本身也不会死锁。
- **崩溃黑匣子**（`TRACER_USE_CRASH`）：预崩溃事件环 + 崩溃现场自动落存储，重启后归档，
  两段式持久化（现场防御性裸写 → boot 可信归档），工厂/现场无人值守诊断用。
- **分级运行日志**（`TRACER_USE_LOG`）：printf 风格、无行长上限、可重入 / ISR 安全，
  push（sink 回调）与 pull（drain）双异步导出接口。
- **离线解析**：`tracer_parser.py` 把 dump 日志 + ELF 还原为符号化调用链，支持在主机上
  用 `.ARM.exidx` 逐帧精确展开（无 exidx 工具链也能精确回溯）。
- **多工具链 / 多内核**：GCC / armclang / IAR / ARMCC；Cortex-M3/4/7/23/33/55/85
  （M0/M0+ 因 Thumb-1 无多寄存器 `PUSH` 不支持）。

## 目录结构

```
tracer.c / tracer.h                     库源码（fault dump、crash、log、parser 调用源）
tracer_crash_store.c/.h                 崩溃黑匣子策略层（介质无关）
tracer_gnugcc.s / tracer_iccarm.s / tracer_armcc.s   工具链汇编入口
tracer_parser.py                        离线解析工具
CMakeLists.txt                          库构建（install/export + find_package）
tests/
  host/            host 单测（CTest：test_tracer / miniprint / crashlog / log / sink /
                   crash_store / parser），-DTRACER_BUILD_TESTS=ON 启用
  qemu/
    application/       uart.h、app.c(共享用例)、startup_armv7m.s/v8m.s、link_mps2.ld
    m3-an385/ m4-an386/ m7-an500/      非 TZ MPS2（M3/M4/M7）
    m33-an505/ m55-an547/ m85-an555/    TZ MPS2/MPS3（M33/M55/M85）
  tool/            board_test.py / test_all.py / fsyntax_check.py /
                   coverage_report / qemu_coverage / merge_coverage / branch_coverage
.github/workflows/  ci.yml / release.yml
```

## 支持范围

| 核心 | QEMU board | machine | 验证状态 |
|---|---|---|---|
| M3 | m3-an385 | mps2-an385 | ✅ QEMU 实测 9/9（无 FPU，`7/8` 跳过） |
| M4 | m4-an386 | mps2-an386 | ✅ QEMU 实测 11/11（含 FPU） |
| M7 | m7-an500 | mps2-an500 | ✅ QEMU 实测 11/11（含 FPU） |
| M33 | m33-an505 | mps2-an505 | ✅ QEMU 实测 11/11（含 FPU） |
| M55 | m55-an547 | mps3-an547 | ✅ QEMU 实测 11/11（含 FPU） |
| M85 | m85-an555 | mps3-an555 | ✅ QEMU 实测 11/11（需含 `mps3-an555` 的 QEMU，如自编译 11.1.50 / 主线 ~11.2+；旧 QEMU 自动降级编译级） |
| M23 | —（无标准 QEMU 板） | — | 🔶 C 层 fsyntax 编译通过；fault 入口汇编为 armv8-M **main** profile（`push {r4-r11}`），M23 baseline 需专用入口 |

> 备注：`m85-an555` 已在含 `mps3-an555` 的 QEMU（自编译 11.1.50，v11.1.0+1168）上
> **实测通过 11/11** —— code@0x0 / RAM@0x21000000 / UART0@0x49303000 假设与 AN555 真机一致；
> 机器缺失时自动降级为编译验证。M0/M0+ 与 M23 运行期 fault dump 的限制见
> [常见问题 FAQ](#常见问题-faq)。

## 快速上手

### 需求

- **arm-none-eabi-gcc**（支持 cortex-m3..m85 的任意版本；用 `CC` 环境变量指定）
- **qemu-system-arm**（标准版即可；用 `QEMU` 环境变量指定）
- Python 3（构建/运行脚本）

### 在本仓库跑测试

```bash
# 全矩阵：QEMU 实测可用机器 + 无机器板自动退化为编译级
CC=arm-none-eabi-gcc QEMU=qemu-system-arm python3 tests/tool/test_all.py

# 单板单用例（构建 + QEMU 运行 + 断言）
CC=arm-none-eabi-gcc QEMU=qemu-system-arm python3 tests/tool/board_test.py m33-an505 0
python3 tests/tool/board_test.py m33-an505 0 --build-only   # 只编译

# 全 core 编译矩阵（fsyntax C + 汇编入口）
CC=arm-none-eabi-gcc python3 tests/tool/fsyntax_check.py

# host 单测（无 ARM 工具链）
cmake -S . -B build-host -DTRACER_BUILD_TESTS=ON
cmake --build build-host && ctest --test-dir build-host
```

每板十一个用例（`TEST_CASE=0..10`，实现在 `tests/qemu/application/app.c`）：`0` smoke、
`1` UsageFault、`2` BusFault、`3` Assert、`4` PSP（线程栈）fault、`5` 重入守卫（dump 内
NMI 抢占）、`6` 自动复位（fault 后二次 boot）、`7` FPU 扩展帧、`8` PSP+FPU 组合、
`9` 重入守卫（dump 内 assert）、`10` 自动复位（assert 后二次 boot）。`7/8` 需 FPU 板
（无 FPU 自动跳过）；无 QEMU 机器板自动退化为编译级。脚本轮询串口输出，**期望标记全部
出现即 PASS**（fault 用例在 dump 后 trap，靠标记早停）。

### 集成到单片机工程

#### 方式 A：CMake（推荐）

```cmake
add_subdirectory(<path-to-this-repo>)            # 仓库根即库
# 强符号接管 startup，必须 whole-archive，否则静态库不被拉入：
target_link_libraries(app PRIVATE
    -Wl,--whole-archive tracer -Wl,--no-whole-archive)
```

`CMakeLists.txt` 按 `CMAKE_C_COMPILER_ID` 自动选汇编入口（GNU/Clang→`tracer_gnugcc.s`，
IAR→`tracer_iccarm.s`，ARMCC→`tracer_armcc.s`），不支持的工具链会 `FATAL_ERROR` 提示。

#### 方式 A2：install + find_package（外部独立工程）

```bash
cmake -S <repo> -B build-tracer \
      -DCMAKE_TOOLCHAIN_FILE=<...>/arm-none-eabi-gcc.cmake \
      -DTRACER_EXIDX_TABLES=ON    # 可选，见“编译选项”
cmake --build build-tracer && cmake --install build-tracer --prefix <prefix>
```

消费工程：

```cmake
find_package(tracer CONFIG REQUIRED)
target_link_libraries(app PRIVATE tracer::tracer)
# 仍需 whole-archive 语义才能覆盖 startup weak：
#   -Wl,--whole-archive -ltracer -Wl,--no-whole-archive
```

头文件装到 `<prefix>/include/tracer/tracer.h`；库本身以 `-std=c99` 编译。

#### 方式 B：Makefile（CubeMX 风格）

- C 源：加入 `tracer.c`；
- 汇编源：按工具链**只加一个** —— gcc 分支 `tracer_gnugcc.s` / iar `tracer_iccarm.s` /
  mdk `tracer_armcc.s`；
- include 加 `-I<path>`；若用 CubeMX 生成的 `stm32f4xx_it.c`，把其中
  `NMI/MemManage/BusFault/UsageFault_Handler` 声明为 `TRACER_WEAK`，避免强符号冲突。

#### 三行接入

```c
#include "tracer.h"

int main(void) {
    tracer_init();              /* 开机初始化（打印 text/stack 范围） */
    /* FreeRTOS 等场景可选覆盖 hooks（打印任务名 + 限任务栈顶），见下 */
    ...
}
```

之后任何 fault 都会自动 dump；fault 后 `tracer_fault_handler` 会 `for(;;)` trap 住。

## 功能详解

### 1. 自动 fault dump（示例输出）

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

dump 全程**关中断**（汇编入口 `cpsid i`）+ **重入保护**（`s_tracer_dumping`）：扫描/打印
期间即使再触发 fault（如 NMI，无法被 `cpsid i` 屏蔽）也只打印 note，绝不递归。

**crash-safe 输出**：定义 `TRACER_PUTCHAR(ch)`（裸 UART 数据寄存器写 / Segger RTT）后，
整个 dump 由库内无锁 mini-printf 逐字符渲染，完全不碰 stdio/printf；未定义时回退
`TRACER_PRINTF`（默认 `printf`，简单但 fault 恰在 printf 内时可能死锁）。

### 2. 主动调用 API

| API | 作用 |
|---|---|
| `tracer_init()` | 开机调用一次，打印版本 + text/stack 范围 |
| `tracer_dump_callstack()` | 主动打印当前调用栈（调试命令用） |
| `tracer_get_callstack(buf, size)` | 当前调用栈 PC 写入缓冲区（落盘/协议发送） |
| `tracer_dump_all()` | 主动打印完整快照（版本 + 任务 + 调用栈 + 轨迹） |
| `tracer_trigger_unalign()` | 主动触发 UsageFault（测 fault 路径） |
| `TRACER_ASSERT(expr)` | 断言：失败打印表达式 + file:line + 调用栈，再自动复位/或 trap |
| `tracer_assert_fail(...)` | `TRACER_ASSERT` 的底层实现（一般不直接调） |

### 3. 可覆盖的 weak hooks

| Hook | 默认 | 用途 |
|---|---|---|
| `tracer_on_fault(&f)` | 空 | fault 时先调用，可打印 FreeRTOS 任务名；`&f` 含 FPU 上下文指针 |
| `tracer_stack_limit()` | 主栈顶 | 返回扫描上界；FreeRTOS 下返回当前任务栈顶 `pxEndOfStack` |
| `tracer_dump_tasks()` | 空 | 列出所有任务（状态/栈水位）；RTOS 适配（FreeRTOS: `vTaskList`），核心保持 RTOS 无关 |
| `tracer_uptime_ms()` | 0 | 系统运行毫秒，每个 dump 打印 `Up:`；FreeRTOS 用 `xTaskGetTickCount()*portTICK_PERIOD_MS` |
| `tracer_watchdog_kick()` | 空 | 喂狗；`TRACER_AUTO_RESET_MS` 延时期间周期调用，防 dump/延时被狗截断 |

### 4. 调用栈回溯方式

| 方式 | 开关 | 适用 | 说明 |
|---|---|---|---|
| 栈扫描（BL/BLX） | 默认兜底 | 所有工具链 | 零依赖；-O2 高度优化下可能 0 帧 |
| `.ARM.exidx` 精确回溯 | `TRACER_USE_EXIDX=1` | GCC/armclang | 需 `-funwind-tables`；-O2 下精确，**推荐** |
| 帧指针链 | `TRACER_USE_FP=1` | A32/IAR | Cortex-M(Thumb) 上仅最内层帧，**不推荐** |
| 动态函数轨迹 | `TRACER_USE_FINSTRUMENT=1` | GCC/armclang | 崩溃前函数进出回放（`+delta` = SysTick 周期），需 `-finstrument-functions` |
| 离线解析 | 事后用脚本 | 所有工具链 | `tracer_parser.py` 用 dump + ELF 符号化；带 PC+SP+原始栈时在主机上用 `.ARM.exidx` 逐帧展开出精确链 |

优先级：`exidx > FP > 扫描`（主动 dump）；fault handler 路径固定用扫描。动态轨迹 /
原始栈 dump 与回溯正交，可同时开启。

### 5. FPU / MVE 上下文解码

当 faulting 上下文使用 FPU/MVE（`EXC_RETURN` bit4=0 = 扩展帧）时，dump 自动打印
`S0..S15 + FPSCR`（hex；离线脚本转浮点）。**实现要点（QEMU 实测）**：

- 硬件只在基本帧上方预留 72 字节扩展区；**lazy stacking**（`FPCCR.LSPEN=1`，RTOS 默认）
  下 S 寄存器值要等 handler 第一次碰 FPU 才真正写入 —— 因此用一条 `vmov s0,s0` 触发
  lazy save，再从 **FPCAR**（`0xE000EF38`）读取；eager（LSPEN=0）时回退读栈上扩展区。
- 需工具链 FPU ISA（GCC `-mfpu` / IAR），否则 `fpu=NULL`。`S16..S31` 为调用者保存
  寄存器，lazy stacking 下异常入口后处理器不再保留 faulting 上下文的值，故不采集。

### 6. 崩溃黑匣子（crash black box，`TRACER_USE_CRASH=1`）

把“宕机现场 + 宕机前发生了什么”自动留下来、重启后读回 —— 工厂/现场无人值守诊断。

- **预崩溃日志**：关键事件处调 `tracer_ring_printf(...)`（无锁、IRQ 安全），RAM 保留最近
  `TRACER_RING_SIZE` 字节。
- **崩溃捕获**：fault/assert/栈溢出 dump 时每个输出字符同时镜像进捕获缓冲（强制走逐字
  mini-printf）；收尾附加 ring 尾 + CRC-32 footer 成一条完整 record，交给 weak
  `tracer_crash_save(data,len)`。
- **两段式持久化**：① 现场（系统不可信）只做**防御性裸写**到保留存储（固定扇区/槽 + CRC，
  极小代码路径，不依赖文件系统）；② 重启后由 boot 代码读回并**归档成文件/上报**。
- **存储分两层**（介质无关策略 + 板级介质）：
  - 策略层 `tracer_crash_store.c`：`tracer_crash_save()` 的通用实现 —— N 槽（默认 2，
    槽 = 一次擦除单位）、双槽交替写、槽头 `'TNC1'|len|crc32`、断电半写不毁旧记录、
    `tracer_crash_store_read_latest()` / `tracer_crash_store_clear()`。介质差异收进 4 个
    weak 原语 `get_media/erase/write/read`（无介质即静默 no-op）。
  - 板级：只实现 4 个介质原语 + boot 归档。mps2-an505 参考 `crash_nv.c`：SPI NOR 顶部保留
    2×4K、littlefs 归档并清除 staging。换板（如 stm32 内部 Flash）只需重写介质原语。
  - QEMU 验证需带 `w25q02jvm` 的补丁版 QEMU + `-drive if=mtd`。
- 内容为**纯文本**：人可读、`tracer_parser.py` 可直接符号化、无私有二进制 ABI。

### 7. 分级运行日志（leveled runtime log，`TRACER_USE_LOG=1`）

`TRACER_USE_CRASH` 与 `TRACER_USE_LOG` 相互独立，可单独开；都开时 `tracer_log()` 与
`tracer_ring_printf()` 写**同一 ring**（合一），崩溃 record 自动含宕机前运行日志。

- **API**：`uint32_t tracer_log(tracer_log_level_t level, const char *fmt, ...)`，级别
  `TRACE/DEBUG/INFO/WARN/ERROR`（0..4）。行格式 `[<ms>][X] <内容>\r\n`（`X` = T/D/I/W/E）。
  `<ms>` 来自 weak `tracer_uptime_ms()` —— **默认已带 SysTick wrap 计数**（SysTick 以
  ~1ms reload 跑时即真实毫秒，免接线），RTOS/应用可覆盖为更准的 tick。
- **分级便捷宏**：`TRACER_LOGT/LOGD/LOGI/LOGW/LOGE`（级别写死在宏名），仍受运行期过滤。
- **运行期分级**（非编译期过滤）：默认 `TRACER_LOG_DEFAULT_LEVEL=INFO`；任意时刻
  `tracer_log_set_level()/tracer_log_get_level()` 调整（调试提 TRACE、正式跑提 WARN）。
- **流式输出**：一次 PRIMASK 临界区内①同步打串口②写共享 ring③每满
  `TRACER_LOG_SINK_CHUNK_SIZE`（默认 128）字节回调一次 weak `tracer_log_sink()`，收尾
  flush 残余块。**调用者无需自行分行**（`\n` 即换行、末尾自动 `\r\n`），超长整段通过、
  三路都不截断；可重入、可在 ISR 调用。
- **异步导出（两选一或都用，不实现则纯同步输出）**：
  - push：覆盖 `tracer_log_sink(const void*, uint32_t)` 追加到文件/flash（临界区内被调，
    只应拷贝/入队，勿阻塞）；
  - pull：后台低优先级任务调 `tracer_log_drain(uint8_t*, uint32_t)` 拿增量字节自行落盘。

## 配置参考

### 编译选项（按需）

- **exidx 精确回溯**：`-DTRACER_EXIDX_TABLES=ON`（省 flash 默认 OFF），再
  `target_compile_definitions(tracer PRIVATE TRACER_USE_EXIDX=1)` —— **必须定义在 tracer
  库目标**（定义在 app 目标不生效）；整个 app 的源也建议 `-funwind-tables` 让回溯链覆盖
  app 帧。
- **动态轨迹**：`-finstrument-functions` 只加到要跟踪的源文件（`set_source_files_properties`），
  并 `target_compile_definitions(tracer PRIVATE TRACER_USE_FINSTRUMENT=1)`。
- 栈扫描 / 离线解析无额外选项，默认即可。

### 内存映射符号

默认取链接器符号（GNU `_stext/_etext/_sstack/_estack`、ARMCC `Image$$...`、IAR
`__section_begin/end`）。链接脚本没有时用 `-D` 覆盖：

```c
-DTRACER_TEXT_START=0x08000000 -DTRACER_TEXT_END=0x08100000
-DTRACER_STACK_BASE=0x20000000 -DTRACER_STACK_TOP=0x20020000
```

### 配置宏一览（tracer.h，均可 `-D` 覆盖）

| 宏 | 默认 | 说明 |
|---|---|---|
| `TRACER_PRINTF` | `printf` | 常规输出函数（可重定向） |
| `TRACER_PUTCHAR` | 无 | **crash-safe** 逐字符输出；定义后 dump 走无锁 mini-printf，不碰 printf |
| `TRACER_STACK_DEPTH` | 32 | 调用栈条目上限 |
| `TRACER_STACK_DUMP_BYTES` | 256 | 原始栈 hex 字节数，0=关闭 |
| `TRACER_USE_EXIDX` | 0 | 启用 .ARM.exidx 精确回溯 |
| `TRACER_USE_FINSTRUMENT` | 0 | 启用动态函数轨迹 |
| `TRACER_TRACE_DEPTH` | 128 | 轨迹环形缓冲大小 |
| `TRACER_USE_FP` | 0 | 帧指针链（Cortex-M 不推荐） |
| `TRACER_FW_VERSION` | `"0.0.0"` | 固件版本串，`tracer_init`/dump 头部打印 |
| `TRACER_AUTO_RESET_MS` | 0 | >0 时 dump 后延时该毫秒数自动系统复位（0=永久 trap） |
| `TRACER_USE_CRASH` | 0 | 崩溃黑匣子：预崩溃环形日志 + dump 镜像 + weak 持久化钩子 |
| `TRACER_USE_LOG` | 0 | 分级运行日志：`tracer_log`/`TRACER_LOGI..` + sink/drain |
| `TRACER_RING_SIZE` | 2048 | 共享环形缓冲（事件与日志共用，字节） |
| `TRACER_CRASH_SIZE` | 8192 | 崩溃 record 捕获缓冲（dump 文本 + ring 尾 + CRC footer） |
| `TRACER_LOG_DEFAULT_LEVEL` | `INFO` | `tracer_log` 初始级别（可运行期调） |
| `TRACER_LOG_SINK_CHUNK_SIZE` | 128 | sink 推送块上限（每满该字节回调一次） |

### 链接

- **必须 whole-archive**（静态库“引用才拉入”，而向量表引用已被 startup 的 weak 满足）。
- ARMCC5 需 `--c99`（否则 `inline` 未定义）。

## 解析 dump 与定位问题

### 解析命令

```bash
python tracer_parser.py <你的.elf> dump.log          # 解析文件
cat dump.log | python tracer_parser.py <你的.elf> -  # 或 stdin
python tracer_parser.py <你的.elf> 0x1000196D 0x100BFB3A   # 直接符号化地址
```

检测到 `arm-none-eabi-addr2line`（或设 `TRACER_ADDR2LINE`）时会附加源码行号
`(file:line)`（-O2 下部分地址显示 `?`）；建议 ELF 保留 `-g` 以获完整行号。

### 输出示例

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

Function trace (last 15):                      ← 崩溃前执行路径
  -> main_task_init → dump_callstack → test5 → ... → main

Raw stack return-address candidates:           ← 栈上残留返回地址
  [80215268] 100019A0  tracer_stack_limit+0x3F

Exidx offline unwind (exact, from PC + raw stack):  ← 主机 .ARM.exidx 展开
  100BFB3A  tracer_trigger_unalign  ->  1000196C  main_task_entry
```

`.ARM.exidx` 离线展开读 ELF 的 `.ARM.exidx/.ARM.extab`（EHABI），从 dump 的
PC/SP/LR + Raw stack 出发在**主机上逐帧展开**，无启发式误报，比扫描法精确（能体现 -O2
内联后的真实调用关系）。展开深度受 `TRACER_STACK_DUMP_BYTES` 限制。这是无 exidx 运行时
（IAR/ARMCC5）获得精确回溯的路径。

### 定位流程

1. **看 PC**：崩溃指令所在函数；`arm-none-eabi-addr2line -e <elf> -f -C <PC>` 拿源码行号。
2. **看异常类型 + 状态寄存器**：`CFSR/MMFSR/BFSR/UFSR` 说明 fault 类型；`BFAR/MMFAR
   [VALID]` 给出错地址；`HFSR=0x40000000 FORCED` 表示低级 fault 升级成 HardFault，根因看 CFSR。
3. **看 Call stack** 定位是谁调的 → 4. **看 Function trace** 弄清“怎么走到这的”（偶发/时序类）
   → 5. **看 Raw stack** 补充扫描漏掉的帧 → 6. 结合调试器对照 `SP` 看内存。

## 测试与覆盖率

覆盖分**两层**，口径要分开看（合并才是 `tracer.c` 的真实覆盖面）：

1. **host 单测**（`tests/host/`，CTest + gcov）—— 纯 C 逻辑 + fault/assert dump 全管线。
   `tracer.c` 的 SCB/CFSR.. 寄存器访问经可选 MMIO 后端（`TRACER_READ32/16/WRITE32`，目标上
   默认仍是直接 MMIO，零变化）；`tests/host/test_fault_handler.c` 用 RAM 数组模拟寄存器 +
   伪造异常帧直接驱动 `tracer_fault_handler()`/`tracer_assert_fail()` 并逐 fault 类型断言。
2. **QEMU 固件实测**（`tests/qemu/`）—— 真异常入口 → 帧解码 → dump → 黑匣子 → trap（或
   自动复位）。每板 11 用例，6 板全矩阵 **66/66 通过**（M85 需含 `mps3-an555` 的 QEMU，
   实机 11/11；M3 无 FPU 自动跳过 `7/8`）。覆盖 host 无法复现的真异常入口、UsageFault
   触发、FPU lazy 帧、任务栈 raw dump。
3. **QEMU 层 gcov**（`tests/qemu/`）—— 固件以 `-fprofile-arcs -ftest-coverage
   -fprofile-info-section` 编译，guest 侧 `tests/qemu/application/gcov_dump.c`（仅该模式链入，
   同时提供强 `tracer_halt` 与 `tracer_stack_limit()=0`）在 trap 前把每个插桩 TU 的 `.gcda`
   以 `[0xA5 'G' 'C'][len][name]` 帧流式吐出 UART；host 按帧切开、逐 (board,case) 用
   `arm-none-eabi-gcov` 归并。自动复位用例（6/10）跳过（复位清零计数器）。

合计口径（本机基线）：

| 层 | `tracer.c` | `tracer_crash_store.c` |
| --- | --- | --- |
| host（`coverage_report.py`） | 92.34%（482/522） | 100%（90/90） |
| QEMU 全 5 板 gcov（`qemu_coverage.py`） | 81.05%（415/512） | —（host 已 100%，不合并） |
| **host ∪ QEMU（`merge_coverage.py`）** | **97.73%（560/573）** | 100% |

行覆盖之外，`branch_coverage.py`（对 host 与 QEMU 的 gcno/gcda 重跑 `gcov -b`）给**分支边
覆盖** —— 每条判断的两个出口都要真实走到才算，故必然低于行覆盖：

| 层（`tracer.c`） | 行覆盖 | 分支覆盖 |
| --- | --- | --- |
| host | 92.34%（482/522） | 77.59%（232/299 边） |
| QEMU | 81.05%（415/512） | 61.54%（160/260 边） |
| **host ∪ QEMU** | **97.73%（560/573）** | **80.17%（275/343 边）** |

`tracer_crash_store.c` host 基线分支 91.67%（66/72）。合并后 `tracer.c` 仅剩 13 行“合理
不可达/专用上下文”行：389（`va_list` 声明计数噪声）、424/425（weak `tracer_crash_save`
空桩）、911–917（SysTick wrap —— QEMU/TCG 下 SysTick 轮询会卡死固件，属真机/RTOS 上下文）、
943/944（`tracer_halt` 的 `for(;;)`，普通固件每次 fault 都执行，仅 QEMU gcov 固件被强
`tracer_halt` 遮蔽）、976（callstack 写缓冲分支，RTOS crash 记录用）、996（`current_sp`
ARM asm 分支，固件 fault 由汇编 trampoline 传 SP）、1394（FPU eager-stacking 兜底）。

## CI 与发布

- **ci.yml**：push/PR 改动根库文件 / `tests/host` / `tests/qemu` / `tests/tool` 时 ——
  ① host 单测（`-DTRACER_BUILD_TESTS=ON` + `ctest`）② 32 位 host（`-m32`，覆盖 load32/walker）
  ③ QEMU 板矩阵（machine 自适应）④ QEMU 层 + host 覆盖率（非阻塞，需 gcc≥13 的
  `-fprofile-info-section`）⑤ 全 core 编译矩阵（`fsyntax_check.py`）。
- **release.yml**：`git tag vX.Y.Z` 触发 —— 跑 CI + 打包源码
  （`dist/tracer-<tag>.tar.gz` + sha256）+ 创建 GitHub Release。库版本（CMake `1.0.0`）
  可按需同步 bump。

## 常见问题 FAQ

1. **`TRACER_USE_EXIDX` / `TRACER_USE_FINSTRUMENT` 必须定义在 tracer 库目标** —— 定义在
   app 目标不会传到单独编译的 tracer.c（宏/回调不生效）。
2. **exidx 还要 tracer 库自身带 `-funwind-tables`**，否则从库内帧回溯直接 0 帧。
3. **帧指针链在 Cortex-M 上只有最内层 1 帧**（GCC Thumb 用 r7 非标准帧布局 +
   `__builtin_return_address` 深度受限）—— 用 exidx 或离线解析。
4. **`-finstrument-functions` 会增大栈占用**，紧凑任务栈可能触发溢出检测；且全量开会记录
   printf 实现（如 LVGL `lv_sprintf_builtin`）刷屏轨迹 —— **务必按文件开启**。
5. **RTOS 任务栈通常在主栈（MSP）之下** —— 扫描/回溯请用 `tracer_stack_limit()` 返回任务
   栈顶，否则跨栈误报。
6. **线程模式 fault 时 xPSR.IPSR=0 是正常的**，别误判；异常类型看 CFSR。
7. **EXC_RETURN bit2=0x4 才是 PSP**（不是 bit1）。
8. **M0/M0+ 不支持**：Thumb-1 无多寄存器 `PUSH`，tracer 入口无法保存 r4..r11。
9. **M23（armv8-M Baseline）**：库 C 层（含 crash/log）可在 `-mcpu=cortex-m23` 编译；
   但 `tracer_gnugcc.s` 的 `push {r4-r11}` 是 main-profile 指令，M23 运行期 fault dump 需
   专用 baseline 入口（高低寄存器组拆开保存）—— 尚未实现，属已知边界。
10. **M85 的 `mps3-an555`** 是 QEMU 主线较晚合入的机器（官方 11.1.0 尚无，约 11.2/
    master+）；本仓库已在含该机器的自编译 QEMU（11.1.50）上**实测通过 11/11**，对缺失
    机器自动降级为编译验证。

## 许可与出处

- **许可**：MIT（见 `LICENSE`）。
- **出处**：本仓库根目录的库源码从 `qemu-embedded-firmware` 的 `libutils/tracer` 移植
  （保持库源码一致）；qemu 板（`tests/qemu`）+ 测试脚本（`tests/tool`）+ CI 为本仓库新写的
  独立发布验证框架。工作日志见原仓库 `works/logs/WORKLOG-2026-09-05-tracer-standalone-repo.md`。
