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
- [ ] 崩溃记录落 Flash + 复位原因（#4，工作量最大，未做）
- [ ] CMake 强制 C99 / install-export（可选）
- [ ] `-funwind-tables` 用 option 控制（可选）

### 备注
- ⚠ 教训（两次）：未提交改动两次被清空（HEAD 均停在 76d30c98）。长工作务必**及时提交**。
- 提交范围（本轮，已提交）：`libutils/tracer/{tracer.c,tracer.h,README.md,tests/CMakeLists.txt,
  tests/test_miniprint.c}`、`boards/mps2-an505/FreeRTOS/application/main.c`、
  `boards/mps2-an505/Core/Inc/uart.h`、`.github/workflows/tracer-ci.yml`、
  `works/logs/WORKLOG-2026-09-02-tracer-industrial.md`。
