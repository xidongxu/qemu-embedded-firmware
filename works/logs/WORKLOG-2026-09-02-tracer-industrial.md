# WORKLOG 2026-09-02 — tracer: 工业级增强（两轮）

## 轮一：代码评审 + 工业级增强 + 修 3 个硬 bug（已提交 76d30c98）
详见该提交 diff 与 repo memory `tracer.md`（2026-09-02 节）。要点：
- 修 3 个 QEMU 实测 bug：`tracer_load32` 未对齐读 Lockup（volatile 化）、EXC_RETURN bit4 极性反、
  FPU 上下文解码位置错（改 FPCAR + lazy-save 触发）。
- P0 重入保护 `s_tracer_dumping` + 汇编 `cpsid i`；`TRACER_FW_VERSION` / `TRACER_AUTO_RESET_MS`
  / `TRACER_ASSERT` / `tracer_dump_all()`；host 测试 + parser 测试 + CI + LICENSE + `tracer::tracer`。

## 轮二：剩余待办落地（⚠ 两次误删丢失，均完整重做并复验）
目标项：#1 crash-safe 输出、#2 看门狗、#3 栈溢出 hook、#5 M0 降级、#7 uptime、#8 finstrument；
#6 S16 捕获判死移除；#4 崩溃落 Flash 未做。

### 实现要点
- **crash-safe 输出（#1）**：`TRACER_PUTCHAR(ch)`。tracer.h 在 PUTCHAR 下
  `#define TRACER_PRINTF tracer_xprintf`（tracer.c 内无锁 mini-printf，支持 %s/%c/%d/%u/%lu/%x/%X
  + '-'/'0'/宽度），不 include stdio → 真正零 printf 依赖；未定义时回退 TRACER_PRINTF（现状）。
  51 处调用零改动。**坑**：曾用 #undef+别名转发 → 宏自递归被 CPP 抑制成隐式声明；改头文件按模式直接定义解决。
- **看门狗（#2）**：weak `tracer_watchdog_kick()`；`tracer_delay_ms`（AUTO_RESET 延时）每 ~64ms 喂；
  纯 trap 不喂（狗做最终兜底）。
- **栈溢出 hook（#3）**：mps2 `vApplicationStackOverflowHook` → `tracer_assert_fail(task, "hook", line)`。
- **uptime（#7）**：weak `tracer_uptime_ms()`，dump/assert/快照打 `Up: N ms`；mps2 FreeRTOS 覆盖
  `xTaskGetTickCount()*portTICK_PERIOD_MS`。
- **finstrument（#8）**：enter/exit 用 PRIMASK 临界区（`tracer_irq_save/restore`，no_instrument_function）。
- **M0 降级（#5）**：头文件/README 明确 "M0/M0+ 不支持（Thumb-1 无多寄存器 PUSH）"。
- **S16 判死（#6）**：QEMU 实测 M33 lazy stacking 下异常入口后 S16–S31 不保留 faulting 值
  （fault 前 S16live=40A00000，handler 内 VSTMIA 读到 0）→ 捕获无意义，已移除并注释原因。
- `Core/Inc/uart.h` 补 `put_char` 声明（原本定义了没声明；它是 PUTCHAR 模式的理想 sink）。

### 验证（重做后全部复验）
- 五配置编译零警告：M33 hard-float / M3 / host / miniprint host / PUTCHAR 模式；parser 单测通过。
- **PUTCHAR QEMU 实测**：BareMetal 临时 `-DTRACER_PUTCHAR=put_char` → dump 输出与 printf 模式逐字
  一致（FW/Up/寄存器/CFSR/Raw stack），验证后 `git checkout` 还原 CMake（注意 PowerShell 改写会丢
  UTF-8 BOM，用 git checkout 最干净）。
- FreeRTOS 重建 + QEMU 冒烟干净启动；BareMetal printf 模式 clean 重建 dump 正常（无 Lockup）。
- 新增 `tests/test_miniprint.c`（mini-printf 格式断言，本机无 host gcc 仅交叉编译 -c 验证，
  CI ubuntu 实跑）+ 挂 CTest/CI。

### 剩余待办
- [ ] 崩溃记录落 Flash + 复位原因（#4，工作量最大，用户决定"先考虑后面再说"）

### 备注
- ⚠ 教训（两次）：未提交改动两次被清空（HEAD 均停在 76d30c98）。长工作务必**及时提交**。
- 提交范围（本轮，已提交）：`libutils/tracer/{tracer.c,tracer.h,README.md,tests/CMakeLists.txt,
  tests/test_miniprint.c}`、`boards/mps2-an505/FreeRTOS/application/main.c`、
  `boards/mps2-an505/Core/Inc/uart.h`、`.github/workflows/tracer-ci.yml`、
  `works/logs/WORKLOG-2026-09-02-tracer-industrial.md`。

---

## 轮三：CMake / CI / 运行时验证收尾（2026-09-02，提交 809e556e + eff5ff00）

选做清单 #2/#3/#4/#5（#1 崩溃落 Flash 用户暂缓）。全部完成并 QEMU/CI 验证。

### #2 CMake 强制 C99 + install/export（提交 809e556e）
- `target_compile_options` 加 `-std=c99`（GNU/Clang/ARMClang），ARMCC 用 `--c99`，仅对 C 翻译单元。
- 新增 `install(TARGETS/EXPORT)`：GNUInstallDirs + CMakePackageConfigHelpers，
  产出 `tracer::tracer` imported target，`find_package(tracer CONFIG)` 可用。
- include 目录改 `BUILD_INTERFACE`（add_subdirectory 用户不变）/`INSTALL_INTERFACE`
  （`${CMAKE_INSTALL_INCLUDEDIR}/tracer`）——**不能写源树绝对路径**（install(EXPORT) 直接报错，
  已踩坑修复）。头文件装 `include/tracer/tracer.h`。
- 验证：两板工程 configure/build 干净；交叉 configure+install 冒烟测试（TRACER_EXIDX_TABLES=ON）
  生成 4 个 cmake 文件；最小 find_package 消费工程 resolves include 路径正确。

### #3 `-funwind-tables` 用 option 控制（提交 809e556e）
- 新 `option(TRACER_EXIDX_TABLES ... OFF)`：默认 OFF，tracer 库不再无条件发 `.ARM.exidx`（省 flash）。
- ⚠ 注意 configure 顺序：根 CMakeLists `add_subdirectory(libutils)`(44) **先于**
  `boards/`(45)，board 无法用 option 提前影响 → FreeRTOS board（用 TRACER_USE_EXIDX 精确回溯）
  在 `target_compile_definitions(tracer PRIVATE TRACER_USE_EXIDX=1)` 旁**显式补
  `target_compile_options(tracer PRIVATE -funwind-tables)`**（宏+flag 一起，无顺序依赖）。
- 验证：build-phone tracer.c 命令含 `-std=c99 -funwind-tables`（exidx 不回归）；
  build-baremetal 无 `-funwind-tables`。

### #4 CI 结果检查 → 修复自引入就红的 host 测试（提交 eff5ff00）
- 现象：tracer-ci 三个 commit 全 failure，失败在 "Build and run host unit tests"（链接阶段）。
- 根因：`tests/test_tracer.c`、`test_miniprint.c` include 整个 `tracer.c`，其非 static 函数引用
  ARM linker-script 符号 `&_estack`/`&_sstack`（`&_stext`/`&_etext`）→ host gcc 链接 undefined。
  （本机无 host gcc/WSL，下载便携 **zig** 用 `zig cc -target x86_64-windows-gnu` 复现并验证。）
- 修复：两个 test include 前钉死 `TRACER_STACK_BASE/TOP`（+miniprint 的 `TRACER_TEXT_START/END`）
  为无害常量（raw dump 已禁用，walker 测试显式传界）。
- 验证：本地 zig(host clang) 编/跑 test_tracer、test_miniprint 全 passed，test_parser.py passed；
  推送后 GitHub Actions **success**（CI 首次全绿）。

### #5 FreeRTOS 运行时触发 assert / stack-overflow（QEMU mps2-an505 实测，未提交，验证后还原）
- **#5a assert**：`main_task_entry` 首行 `TRACER_ASSERT(0)` → dump：
  `Tracer: Assert Failed / Up: 5 ms / Assert: 0 / End of assert (trapped)`（uptime override 生效）。
- **#5b stack-overflow**：深递归触发 FreeRTOS method-2 失败——**heap 分配的栈 pxStack 与 TCB
  在 heap 紧邻（TCB 低地址），bomb 穿栈底先覆盖 TCB 的 pxStack/pxTopOfStack 字段 → method2
  读损坏指针判 canary==0xa5 → 假阴性，hook 不触发**（SP 打印证明 bomb 已穿栈底 4KB）。
- 可靠触发法：main_task 改 `xTaskCreateStatic`（静态栈 .bss，TCB 分离）+ 直接写坏
  pxStack[0..3]（真实溢出的 canary 破坏机制）→ vTaskDelay 让出 → method2 触发 → dump：
  `Assert Failed / Up: 0 ms / Assert: main_task / At: vApplicationStackOverflowHook:309 /
  Call stack: 100BFBC4 100C006C`。
- 结论/记录：静态栈+canary 破坏是可靠测试法；heap 栈深溢出会砸 TCB 属 FreeRTOS method2 已知边界
  （真机可靠检测建议 `uxTaskGetStackHighWaterMark` 水位告警 或 xTaskCreateStatic 关键任务）。
- ⚠ 验证全程 `git checkout -- main.c` 还原，无测试残留提交。
