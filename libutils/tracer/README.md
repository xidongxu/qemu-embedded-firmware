# tracer — minimal Cortex-M fault dump library (M3..M85) + per-core QEMU test matrix

`tracer` 是一个自包含（无 CMSIS / RTOS / printf 依赖）的 Cortex-M 故障诊断库：
fault/assert dump、调用栈、崩溃黑匣子（可选 crash/log），并通过 **每核一块 QEMU board**
的测试矩阵验证 **Cortex-M3 → M85** 全架构。本仓库同时作为该库的**独立发布源**
（CMake 可 install/export + `find_package(tracer)` + CI/CD 发布）。

## 目录结构

```
库源码位于仓库根：tracer.c/.h、tracer_crash_store.{c,h}、3 工具链汇编入口
（tracer_gnugcc.s / tracer_iccarm.s / tracer_armcc.s）、tracer_parser.py、CMakeLists.txt
host-tests/           host 单测（CTest：test_tracer / miniprint / crashlog / log / sink / crash_store / parser）
qemu-tests/
  application/            uart.h(CMSDK APB UART)、app.c(共享测试)、startup_armv7m.s/v8m.s、link_mps2.ld
  m3-an385/   m4-an386/   m7-an500/   非 TZ MPS2（M3/M4/M7）
  m33-an505/  m55-an547/  m85-an555/   TZ MPS2/MPS3（M33/M55/M85）
# host 单测（host-tests） vs QEMU 板测（qemu-tests）：两层覆盖，口径见『测试与覆盖率』
scripts/             board_test.py(构建+QEMU+断言)、test_all.py(矩阵)、fsyntax_check.py(全核编译)
.github/workflows/   ci.yml(host 单测+矩阵+fsyntax)、release.yml(tag 发布)
```

## 需求工具

- **arm-none-eabi-gcc**（任何支持 cortex-m3..m85 的版本；`CC` 环境变量指定）
- **qemu-system-arm**（标准版即可；`QEMU` 环境变量指定）
- Python 3（构建/运行脚本）

## 快速开始

```bash
# 全矩阵：QEMU 实测可用机器 + 无机器板自动退化为编译级
CC=arm-none-eabi-gcc QEMU=qemu-system-arm python3 scripts/test_all.py

# 单板单用例（构建+QEMU 运行+断言）
CC=arm-none-eabi-gcc QEMU=qemu-system-arm python3 scripts/board_test.py m33-an505 0
python3 scripts/board_test.py m33-an505 0 --build-only   # 只编译

# 全 core 编译矩阵（fsyntax C + 汇编入口）
CC=arm-none-eabi-gcc python3 scripts/fsyntax_check.py
```

每板十一个用例（`TEST_CASE=0..10`，实现见 `qemu-tests/application/app.c` 头注释）：
`0` smoke、`1` UsageFault、`2` BusFault、`3` Assert、`4` PSP（线程栈）fault、
`5` 重入守卫（dump 内 NMI 抢占）、`6` 自动复位（fault 后二次 boot）、
`7` FPU 扩展帧、`8` PSP+FPU 组合、`9` 重入守卫（dump 内 assert）、
`10` 自动复位（assert 后二次 boot）。`7/8` 需 FPU 板（config `fpu`，
无 FPU 板自动跳过）；无 QEMU 机器板自动退化为编译级。
脚本启动 QEMU 后轮询串口输出，**期望标记全部出现即 PASS**（fault 用例在 dump 后 trap，靠标记早停）。

## 支持矩阵（标准 QEMU 实测）

| 核心 | board | QEMU machine | 本仓库状态 |
|---|---|---|---|
| M3  | m3-an385  | mps2-an385  | ✅ QEMU 实测 9/9（无 FPU，`7/8` 跳过） |
| M4  | m4-an386  | mps2-an386  | ✅ QEMU 实测 11/11（含 FPU） |
| M7  | m7-an500  | mps2-an500  | ✅ QEMU 实测 11/11（含 FPU） |
| M33 | m33-an505 | mps2-an505  | ✅ QEMU 实测 11/11（含 FPU） |
| M55 | m55-an547 | mps3-an547  | ✅ QEMU 实测 11/11（含 FPU） |
| M85 | m85-an555 | mps3-an555  | 🔶 编译级 11/11（实测 QEMU 11.1.0 仍无 `mps3-an555`；需含 an555 的更新版本 —— master/~11.2+；脚本检测缺失自动 build-only） |
| M23 | —（无标准 QEMU 板） | — | 🔶 C 层 fsyntax 编译通过；fault 入口汇编为 armv8-m **main** profile（`push {r4-r11}`），M23 baseline 需专用入口（见下） |

> 说明：`m85-an555` 的链接布局与 UART 基址沿用 `m55-an547` 假设，标 **pending**——拿到含 `mps3-an555`
> 的 QEMU 后，请先核对该板 UART 基址/内存映射再启用运行用例。

## 已知限制 / 边界

- **M0/M0+ 不支持**：Thumb-1 无多寄存器 PUSH，tracer 入口无法保存 r4..r11。
- **M23（armv8-M Baseline）**：库 C 层（含 crash/log）可在 `-mcpu=cortex-m23` 编译；
  但 `tracer_gnugcc.s` 的 `push {r4-r11}` 是 main-profile 指令，M23 上运行期 fault dump
  需要一个**专用 baseline 入口**（把高低寄存器组拆开保存）——尚未实现，属已知边界。
- M85 的 `mps3-an555` 是 QEMU 主线较晚才合入的机器（官方 **11.1.0 实测尚无**，约 11.2/master+）；本仓库脚本对缺失机器自动降级为编译验证。

## CI / 发布（GitHub Actions）

- **ci.yml**：每次 push/PR（改动根库文件/host-tests/qemu-tests/scripts 时）——
  ① host 单测（`cmake -DTRACER_BUILD_TESTS=ON` + `ctest`，含解析器）② 32 位 host
  单测（`-m32`，覆盖 32 位限定的 load32/walker 路径）③ QEMU 板矩阵
  （`test_all.py`，QEMU machine 自适应）④ 全 core 编译矩阵（`fsyntax_check.py`）。
- **release.yml**：`git tag vX.Y.Z` 触发 —— 跑 CI + 打包源码（`dist/tracer-<tag>.tar.gz`）
  并创建 GitHub Release。版本从 tag 取值（CMake 库版本 `1.0.0` 可按需同步 bump）。

## 测试与覆盖率

覆盖分**两层**，口径要分开看（合起来才是 `tracer.c` 的真实覆盖面）：

1. **`host-tests/`（host，CTest + gcov 行覆盖）** —— 纯 C 逻辑 + **fault/assert
   dump 全管线**。`tracer.c` 的 SCB/CFSR..BFAR/UFSR 寄存器访问经可选的 MMIO 后端
   （`TRACER_READ32/16/WRITE32`，目标上默认仍是直接 MMIO，行为零变化），
   `host-tests/test_fault_handler.c` 用 RAM 数组模拟寄存器 + 伪造异常帧，直接驱动
   `tracer_fault_handler()`/`tracer_assert_fail()`，并对每类 fault 的输出值断言。
   当前基线（本机 MinGW gcc 跑 `python scripts/coverage_report.py`）：`tracer.c`
   **92.3%**、`tracer_crash_store.c` **100%**。`tracer.c` 剩余 40 行均为 host
   **物理不可覆盖**，按归属：
   - ~16 行 32 位 host 限定（load32 / BL·BLX walker）→ CI `-m32` job；
   - ~22 行真硬件上下文（`trigger_unalign` 读 0x3 触发真 UsageFault、FPU 扩展帧
     打印 `f.fpu!=NULL`、raw-stack 有数据的字节 dump、`stack_limit()==0` 兜底）
     → QEMU 板测；
   - ~2 行真机边界（up-time 非零的多位时间戳前缀、真 trap 死循环体）。
2. **`qemu-tests/`（QEMU 固件实测）** —— 硬件/fault 上下文路径：真异常入口 → 帧解码 →
   dump 输出 → 黑匣子落盘 → trap（或自动复位）；每板 11 用例（smoke / UsageFault /
   BusFault / Assert / **PSP 线程栈 fault** / **重入守卫 NMI+assert** /
   **自动复位（fault+assert）** / **FPU 扩展帧** / **PSP+FPU 组合**），当前 5 板矩阵
   **66/66 通过**（M85 编译级 11/11；M3 无 FPU 自动跳过 `7/8`）。它覆盖 host 无法
   复现的真异常入口、UsageFault 触发、FPU lazy 帧、可寻址任务栈的 raw dump。
3. **`qemu-tests/` QEMU 层 gcov（Zephyr 式导出）** —— 固件以
   `-fprofile-arcs -ftest-coverage -fprofile-info-section` 编译，guest 侧
   `qemu-tests/application/gcov_dump.c`（只在该模式下链入，同时提供强 `tracer_halt`
   与 `tracer_stack_limit()=0`）在 trap 前把每个插桩 TU 的 `.gcda` 以
   `[0xA5 'G' 'C'][len][name]` 帧流式吐出 UART；host 按帧切开、用
   `arm-none-eabi-gcov` 逐 (board,case) 归并。跑 `python scripts/qemu_coverage.py`
   得 QEMU 层行覆盖，`python scripts/merge_coverage.py` 与 host 取并集。自动复位用例
   （6/10）会被跳过（复位会清零计数器）。

合计口径（本机基线）：

| 层 | `tracer.c` | `tracer_crash_store.c` |
| --- | --- | --- |
| host（`coverage_report.py`） | 92.34%（482/522） | 100%（90/90） |
| QEMU 全 5 板 gcov（`qemu_coverage.py`） | 81.05%（415/512） | —（host 已 100%，不合并） |
| **host ∪ QEMU（`merge_coverage.py`）** | **97.73%（560/573）** | 100% |

行覆盖之外，`python scripts/branch_coverage.py`（对 host 与 QEMU 的 gcno/gcda 重跑
`gcov -b`）给**分支边覆盖**——每个 if/else/switch/三元/短路的出口都要真实走到才计，
所以必然低于行覆盖：

| 层（`tracer.c`） | 行覆盖 | 分支覆盖 |
| --- | --- | --- |
| host | 92.34%（482/522） | 77.59%（232/299 边） |
| QEMU | 81.05%（415/512） | 61.54%（160/260 边） |
| **host ∪ QEMU** | **97.73%（560/573）** | **80.17%（275/343 边）** |

并集后仍有 68 条分支边没走到"第二个出口"（以防御/错误路径为主）；
`tracer_crash_store.c` host 基线分支 91.67%（66/72）。注意"行覆盖到 100%"≠"分支覆盖
到 100%"——每条判断的两条边都要真实走过才算，这正是分支口径比行口径难的原因。

`tracer.c` 合并后只剩 13 行（都是"合理不可达/专用上下文"）：389（`va_list` 声明行计数
噪声，函数体已覆盖）、424/425（weak `tracer_crash_save` 空桩的 `(void)` 语句）、
911–917（`tracer_uptime_ms` 的 SysTick wrap 分支——QEMU/TCG 下 SysTick 轮询实测会使固件
卡死，该分支属真机/RTOS 上下文）、943/944（`tracer_halt` 的 `for(;;)`——仅 QEMU gcov
固件被 `gcov_dump.c` 强 `tracer_halt` 遮蔽，普通固件每次 fault 都会执行）、976
（callstack 写缓冲分支，RTOS crash 记录用）、996（`current_sp` 的 ARM asm 分支，固件
fault 全程由汇编 trampoline 传 SP）、1394（FPU eager-stacking 兜底，QEMU 各 FPU 板
FPCAR 均非 0）。`tracer_crash_store.c` 由 host 单测 100% 覆盖（RAM/MMIO 后端）。

CI 会跑 host coverage（`gcov` 聚合，artifact `coverage-report`）与 QEMU 层 coverage
（`qemu_coverage.py` + `merge_coverage.py`，非阻塞、需 gcc≥13 的
`-fprofile-info-section`，artifact `qemu-coverage`）。

## 以库方式使用

```cmake
# 方式 A：add_subdirectory（本仓库根即库，CMakeLists.txt 在根）
add_subdirectory(<path-to-this-repo>)
target_link_libraries(app PRIVATE tracer)          # 或 tracer::tracer

# 方式 B：find_package（先 cmake --install）
find_package(tracer CONFIG REQUIRED)
target_link_libraries(app PRIVATE tracer::tracer)
```

fault 向量为 tracer 导出的强符号，覆盖工程 startup 的 weak 默认；
若以静态库链接需 whole-archive（库级细节文档见 `LIBRARY.md`，为上游库 README 的移植副本）。

## 从何处来

本仓库根目录的库源码从 `qemu-embedded-firmware` 的 `libutils/tracer` 移植（保持库源码一致），
board/scripts/CI 为新写的独立发布验证框架。工作日志见原仓库
`works/logs/WORKLOG-2026-09-05-tracer-standalone-repo.md`。
