# 实验 07 — RTP / PCMU 媒体流（pj_rtp_test.c）

**阶段**：媒体平面（stage 6）。

## 目的
验证"通话真正传声音"的媒体管线：**G.711 编解码 + RTP 打包/解包 + UDP 收发**。06 解决了信令（怎么拨通），07 解决媒体（拨通后传什么、怎么传）。

## 思路（UDP 回环）
1kHz 正弦 → PCMU 编码 → 打成 RTP 包（pt=0, ssrc 固定）→ UDP 发到**本机自己的 socket** → 读回 → 解包 → PCMU 解码 → 校验波形（有能量、主频 ~1kHz）。

## 流程
1. 生成 1kHz/8kHz 正弦（16bit PCM）。
2. `alaw_ulaw` 查表：PCM→μ-law（编码）。
3. `pjmedia_rtp_session` 打 RTP 头（版本、PT、seq、timestamp、ssrc），UDP `sendto` 到本机 `:15064`。
4. 同一任务 `recvfrom` 读回，`pjmedia_rtp_session` 解包，μ-law→PCM（解码）。
5. 校验：回波有能量、主频 ~1kHz（定点能量 / 过零计数）。

## 学到什么
- **RTP 协议**（RFC 3550）：固定头 12 字节（V/P/X/CC/M/PT/seq/timestamp/ssrc）、载荷类型(PT)、时间戳按采样率递增（8kHz 下每 10ms 帧 = 160 采样？此处每帧 80 采样=10ms@8kHz）。
- **G.711 PCMU**：μ-law 8bit 压缩，80 采样 10ms 帧 = 80 字节载荷。
- `pjmedia_rtp_session` 的打包/解包 + 序列号/时间戳处理。
- UDP socket 上 RTP 的收发。

> 说明：09 里 `frm_per_pkt=1`（每包 1 帧 10ms）与这里"每帧一个 RTP 包"一脉相承——G.711 默认 2 帧/包（20ms）会让 jitter buffer 消耗翻倍导致静音。

## 如何启动
默认构建（`pj_rtp_test_run()` 在 06 之后执行）。

## 成功标准
串口打印 `pj_rtp: PASSED`（RTP/PCMU 回环波形校验通过）。
