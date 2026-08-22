# 实验 08 — 全双工通话媒体链路（pj_call_test.c）

**阶段**：完整通话媒体（stage 8）。

## 目的
在 06（信令）+ 07（单方向媒体）之上，验证**真实双向通话的媒体链路**：`mic → PCMU 编码 → RTP → UDP → RTP 解码 → PCMU 解码 → speaker`，且**两个方向同时进行**（全双工）。

## 思路
板上模拟**两个端点 A/B**，各自一条 RTP/PCMU 通道：
- 端点 A（主叫）：RX `:15066`，TX → `:15068`
- 端点 B（被叫）：RX `:15068`，TX → `:15066`

方向 A→B 用采集缓冲 capA → 播放缓冲 playB；方向 B→A 用 capB → playA。两个方向独立校验（信号存在 + 峰值）。

## 流程
1. QEMU 喂 mic WAV（`-global mpsx-simple-mic.infile=<wav>`）：采集 A 音频。
2. A 方向：mic(PCM) → PCMU 编码 → RTP → UDP(15066→15068) → 收 → RTP 解包 → PCMU 解码 → 播放缓冲。
3. B 方向：同一条链路反向（可用另一段 WAV 或同一段）。
4. 校验两端播放缓冲：有信号、峰值达到阈值。
5. 用自写 `pj_rtcp_engine`（RFC 3550 子集）统计收发包/丢包/RTT。

## 学到什么
- 一条通话链路的**完整数据流**（采样→压缩→封装→传输→解封装→解压→播放）。
- **全双工**：收发独立通道、独立缓冲。
- 在真实通话里，编码帧大小、RTP 时间戳节奏、接收端抖动缓冲如何配合。
- 自写 **RTCP 引擎**（`pj_rtcp_engine.c`）如何用 SR/RR 算丢包与 RTT（NTP 字段用 tick 代替）。

## 如何启动
默认构建（`pj_call_test_run()` 最后执行）。运行需 mic + audio：
```
qemu-system-arm.exe -machine mps2-an505,audiodev=a0 -cpu cortex-m33 -m 16M -display none -serial stdio `
  -audiodev wav,path=out.wav,id=a0 `
  -global mpsx-simple-mic.infile=testcase\sine_1k_8k.wav `
  -kernel build\...\an505-qemu.elf
```

## 成功标准
串口打印 `pj_call: PASSED`（两个方向播放峰值达标）。
