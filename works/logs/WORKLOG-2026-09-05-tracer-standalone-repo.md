# WORKLOG 2026-09-05 — 独立 tracer 发布仓库（每核 QEMU board 矩阵 + CI/CD 发布）

> 目标：把 libutils/tracer 提为独立可发布项目 `C:\Users\xidon\code\github\tracer`，支持
> ① 库管理发布 ② CI/CD ③ cortex-m3→m85 各架构 board + 标准 qemu-system-arm 编译测试。
> 决策（askQuestions）：仓名 `tracer`；M85 升级新 QEMU(11.x) 本地实测；M23 编译级；完整搭建。

## 可行性核验（先于搭建）
- 标准 QEMU **9.0.0** machine 覆盖：M3(mps2-an385/an511)、M4(an386)、M7(**an500**)、M33(an505/an521/an524/musca)、M55(**an547**)；**无 M23**、**无 M85**。
- QEMU 主线 11.1.50 docs 已有 **mps3-an555 = Cortex-M85**（AN555，无 PACBTI）→ M85 需新 QEMU。
- M23 任何标准 QEMU 均无板 → 编译级。

## 搭建成果
- 库 `tracer/`：从 qemu-embedded-firmware/libutils/tracer **逐字节移植**（Copy-Item），含 crash-store/parser/host tests/CMake install-export 原样可用。
- `boards/_common/`：自包含无 CMSIS 框架——
  - `uart.h`：CMSDK APB UART（DATA+0/STATE+4/CTRL+8，`board_putc`）作 TRACER_PUTCHAR sink；
  - `app.c`：共享测试（TEST_CASE 0=smoke/callstack、1=UsageFault、2=BusFault），打印走 `tracer_log`（**ring_printf 只进 ring 不上串口**，是调试踩坑点）；
  - `startup_armv7m.s`/`startup_armv8m.s`：极简向量表+Reset（.data copy/.bss zero/VTOR），**base-profile 兼容指令**（M23 需要 `adds` 不能 `add`/不能用 `ldr rn,[rm],#4` 写回）；
  - `link_mps2.ld` + 每板 ld：需导出 `_stext/_etext/_sstack/_estack/_sidata`。
- 板布局（QEMU 实测得到）：非TZ an385/386/500 code@0x0 RAM@0x20000000 UART@0x40004000；an505 code@0x10000000 RAM@0x38000000 UART@**0x40200000**；an547 code@0x0(boot512K) RAM@0x21000000 UART@**0x49303000**。
- scripts：`board_test.py`（build+QEMU+**轮询期望标记早停**）、`test_all.py`（machine 缺失自动 build-only）、`fsyntax_check.py`（M3..M85+M23：fsyntax tracer.c + main-profile 汇编入口 -c）。
- CI：ci.yml（host 单测 ctest + test_all + fsyntax）；release.yml（tag v* → 打包 dist/tracer-<tag>.tar.gz + sha256 + GitHub Release）。

## 验证结果（本机 CC=arm gcc 15.3, QEMU=标准 9.0.0）
- **QEMU 实测 5 板 × 3 用例 = 15/15 PASS**：m3-an385 / m4-an386 / m7-an500 / m33-an505 / m55-an547（smoke/UsageFault/BusFault；BusFault dump 全字段正确：BFSR=0x82、BFAR、EXC_RETURN、raw stack、CRC record）。
- m85-an555：完整 build（含 asm）通过；run 待含 `mps3-an555` 的新 QEMU（config `build_only`，脚本自动跳过 run）。
- M23：tracer.c C 层 `-mcpu=cortex-m23` fsyntax **PASS**；但 `tracer_gnugcc.s` 的 `push {r4-r11}` 是 armv8-m **main** profile，M23(baseline) 汇编不过 → **fault 运行期入口需专用 baseline port**（已注明 README/矩阵）。
- 链接坑（已修）：`.data` LMA 自引用重叠→ `__etext`/`_sidata` 显式；**link 命令必须带 -mcpu/-mthumb** 选对 Thumb multilib（否则 "Unknown destination type (ARM/Thumb)" 调 ARM 版 libgcc）；用 `--specs=nano.specs`（勿 -nostdlib）。
- 打印坑：`tracer_ring_printf` 仅进崩溃 ring，**不上串口**；应用 marker 用 `tracer_log()`。

## 首次提交
`616d461`（本地 git main，未 push）。用户后续：升级 QEMU 到含 an555 的 11.x 后跑通 m85 运行用例；需要时 `git remote add` + push。
