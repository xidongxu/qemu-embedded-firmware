# pjlab — PJ (pjproject / PJSIP) 学习实验工坊

> 位置：`boards/mps2-an505/FreeRTOS/pjlab/`
> 平台：mps2-an505 (Cortex-M33) + FreeRTOS + lwIP + QEMU

这是 `application/` 下所有 `pj_*` 实验的**整理、文档与源码副本**。这些实验按"自底向上"的顺序，一步步把一个完整的 SIP 语音通话（信令 + 媒体）在嵌入式上跑通，非常适合学习 pjproject 的层次结构。

## 目录结构

```
pjlab/
├── CMakeLists.txt      # 收集 src/ 实验源码编入固件 target
├── README.md          # 本文件：总览 / 构建 / 运行 / 速查表
├── docs/              # 每个实验的详细文档（目的/思路/流程/学什么/运行/成功标准）
│   ├── 01_pjlib_port.md        # PJLIB 移植自测
│   ├── 02_socket_ioqueue.md    # 网络 socket + ioqueue
│   ├── 03_sip_register.md      # SIP REGISTER 注册
│   ├── 04_pjmedia_framework.md # pjmedia 框架 + G.711 编解码
│   ├── 05_dsp_aec_conf.md      # DSP：回声消除 + 会议桥混音
│   ├── 06_sip_invite.md        # SIP INVITE 会话 + SDP 协商
│   ├── 07_rtp_media.md         # RTP / PCMU 媒体流
│   ├── 08_call_media.md        # 全双工通话媒体链路
│   └── 09_dual_call.md         # 双 QEMU / 与宿主互通真实通话
└── src/               # 实验真实源码（pj_*.c / pj_*.h，编译用）
```

> ✅ `src/` 是**实验的真实源码**（从 `application/` 迁入）。`application/` 现在**保留给应用开发**。构建时 `pjlab/CMakeLists.txt` 把这些源码编入同一固件 target，`main.c`（application/）仍按顺序调用实验入口（`pj_*_test_run()`）。

## 实验分层与依赖关系

```
pjlib (os/pool/thread/mutex/timer/sock/ioqueue)   <- 01 pj_test, 02 pj_net_test
  └── pjsip (endpoint/transport/dialog/inv/reg)    <- 03 pj_sip_test, 06 pj_sip_inv_test
        └── pjmedia (endpoint/codec/rtp/rtcp/stream) <- 04 pj_media_full_test, 05 pj_media_dsp_test
              └── 通话集成 (信令 + 媒体一起)          <- 07 pj_rtp_test, 08 pj_call_test, 09 pj_sip_dual_test
```

## 如何构建

工具链：`arm-none-eabi-gcc 15.3.1`，CMake + Ninja。在**固件仓库根目录**执行：

```powershell
# 1) 默认工程（不设 PJ_DUAL_ROLE = non-dual）：跑「全部单机实验」（01~08
#    按顺序自动执行）。若 build/ 之前以 dual 模式构建过，先删除
#    build/CMakeCache.txt（或用新目录）重新 configure。
cmake -B build -S . -G Ninja `
  -DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe `
  -DCMAKE_TOOLCHAIN_FILE=C:/Users/xidon/code/github/qemu-embedded-firmware/cmake/arm-none-eabi-gcc.cmake `
  -DBOARD=mps2-an505 -DPROJECT=FreeRTOS
cmake --build build

# 产物：build/boards/mps2-an505/FreeRTOS/an505-qemu.elf

# 2) 双 QEMU 通话实验（09）：分别构建 caller / callee
cmake -B build-caller -S . -G Ninja ... -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DPJ_DUAL_ROLE=caller
cmake --build build-caller
cmake -B build-callee -S . -G Ninja ... -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DPJ_DUAL_ROLE=callee
cmake --build build-callee

# 3) 与宿主 pjsua 互通（09 变体）：caller + PJ_HOST_CALL
cmake -B build-hostcall -S . -G Ninja ... -DPROJECT=FreeRTOS -DPJ_DUAL_ROLE=caller -DPJ_HOST_CALL=ON
cmake --build build-hostcall
```

（`...` 为与 1) 相同的 `-G/-DCMAKE_MAKE_PROGRAM/-DCMAKE_TOOLCHAIN_FILE` 等参数。）

## 如何运行（QEMU）

QEMU（补丁版）：`C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe`

**默认实验（01~08）单实例：**

```powershell
qemu-system-arm.exe -machine mps2-an505 -cpu cortex-m33 -m 16M `
  -display none -serial stdio -kernel build\boards\mps2-an505\FreeRTOS\an505-qemu.elf
```
串口会依次打印每个实验的 `PASS/FAIL`。

**08 pj_call_test 需要 mic/audio 设备**（喂 WAV + 录输出）：

```powershell
qemu-system-arm.exe -machine mps2-an505,audiodev=a0 -cpu cortex-m33 -m 16M `
  -display none -serial stdio `
  -audiodev wav,path=out.wav,id=a0 `
  -global mpsx-simple-mic.infile=testcase\sine_1k_8k.wav `
  -kernel build\...\an505-qemu.elf
```

**09 双 QEMU 通话**：见 `docs/09_dual_call.md`（两个 QEMU 实例互拨）或与宿主 pjsua 互通（`works/tools/run_hostcall_test.ps1`）。

## 实验速查表

| # | 源码 | 入口 | 验证对象 | 成功标志 |
|---|------|------|---------|---------|
| 01 | `pj_test.c` | `pj_test_run()` | PJLIB OS 移植（线程/互斥/信号量/原子/定时器/内存池） | `pj_test: ALL PASSED` |
| 02 | `pj_net_test.c` | `pj_net_test_run()` | PJLIB socket/ioqueue/select over lwIP | `pj_net: PASSED` |
| 03 | `pj_sip_test.c` | `pj_sip_test_run()` | PJSIP endpoint/transport/REGISTER 完整事务 | `pj_sip: PASSED` |
| 04 | `pj_media_full_test.c` | `pj_media_full_test_run()` | pjmedia 框架启动 + G.711 编解码 + RTCP 会话 | `pj_media_full: PASSED` |
| 05 | `pj_media_dsp_test.c` | `pj_media_dsp_test_run()` | AEC 回声消除 + 会议桥混音 | `pj_media_dsp: PASSED` |
| 06 | `pj_sip_inv_test.c` | `pj_sip_inv_test_run()` | INVITE 会话 UAC/UAS + SDP 协商 → CONFIRMED | `pj_sip_inv: PASSED` |
| 07 | `pj_rtp_test.c` | `pj_rtp_test_run()` | G.711 + RTP 打包/解包 + UDP loopback | `pj_rtp: PASSED` |
| 08 | `pj_call_test.c` | `pj_call_test_run()` | 双向通话媒体链路（mic→PCMU→RTP→解码→speaker） | `pj_call: PASSED` |
| 09 | `pj_sip_dual_test.c` | `pj_sip_dual_test_run()` | 双 QEMU / 与宿主 真实 SIP 通话（信令+媒体+DTMF） | `media ALL PASSED` / `media ALL PASSED` |

## 推荐学习路径

1. 先跑 **默认构建**，看串口按顺序打印 01→08 的 PASS —— 建立"自底向上"的整体感。
2. 从 `docs/01` 开始逐个读文档 + 源码，理解每一层。
3. 最后跑 `docs/09` 的双 QEMU / 宿主互通，看完整通话。
4. 想进阶做**真实电话应用**时，改用 pjproject **高层 API（PJSUA）** 重写应用层 —— 底层机制已在 pjlab 学透。
