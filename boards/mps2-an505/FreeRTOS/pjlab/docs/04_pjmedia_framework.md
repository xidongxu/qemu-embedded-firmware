# 实验 04 — pjmedia 框架 + G.711 编解码（pj_media_full_test.c）

**阶段**：完整 pjmedia 媒体栈（stage 14）。

## 目的
验证 **pjmedia** 媒体框架（endpoint / codec manager / G.711 / event / RTCP）编译进来后能真正启动，且内置 **G.711（μ-law）编码器**能实际编码/解码。

## 思路
打通"编码器真能出声"这一层：生成 1kHz 正弦 → PCMU 编码 → 解码回 → 校验能量与主频。这是后面 RTP 里真正传输的载荷。

## 流程
1. `pjmedia_endpt_create/destroy`：媒体 endpoint 生命周期。
2. `pjmedia_codec_g711_init` + codec manager：注册并 `pjmedia_codec_mgr_find_codecs` 找到 PCMU。
3. **编码/解码往返**：1kHz 正弦 16bit → 分配 codec → `codec->op->encode`（PCM→μ-law）→ `decode`（μ-law→PCM）→ 校验回波能量/主频。
4. 事件管理器（endpt 自带）。
5. `pjmedia_rtcp_session` 初始化 + 统计（tx/rx 喂包，loss/jitter 计算）。

## 学到什么
- pjmedia 的**架构**：endpoint 持有 ioqueue/pool/codec mgr；媒体端口(port)/codec/传输是独立组件。
- **G.711 μ-law 编解码**：`alaw_ulaw` 查表压缩，PCM(16bit)↔μ-law(8bit)。
- 编解码器框架：`pjmedia_codec_op`（alloc/open/encode/decode/close）、codec param（`frm_per_pkt`、`vad` 等——实验 09 里 `frm_per_pkt=1` 就改这里）。
- RTCP 会话：RFC 3550 收发统计。

## 如何启动
默认构建（`pj_media_full_test_run()` 在 03 之后执行）。

## 成功标准
串口打印 `pj_media_full: PASSED`。
