# 2026-08-19 工作记录：RTCP（Stage 13）+ 全量 pjmedia 框架接入（Stage 14）

> 本文档整理两天（2026-08-17→08-19）PJSIP 通话链路的两个新里程碑：
> - **Stage 13**：为实时通话加上 **RTCP 监控**（丢包率 / 抖动 / RTT），自研精简 RFC 3550 引擎并验证双向自洽。
> - **Stage 14**：把 **pjmedia 全套媒体框架拉入编译**（endpoint / codec mgr / G.711 / stream / RTCP / event / 会议等 ~50 文件），通过全量自测与双 QEMU 通话回归，**功能零退化**。
>
> 重点整理：**pjproject 当前使用了哪些功能、哪些未参与编译、已测功能可实现什么应用、未编部分主要用于什么场景**（见第四节起）。
>
> 对应代码：本次变更**尚未提交**。涉及文件：
> - 新增 `FreeRTOS/application/pj_rtcp_engine.c/.h`（Stage 13 精简 RTCP）
> - 新增 `FreeRTOS/application/pj_media_full_test.c/.h`（Stage 14 全量自测）
> - 新增 `ports/freertos/src/audiodev_stub.c`（音频子系统空桩）
> - 修改 `ports/freertos/CMakeLists.txt`（pjmedia 目标扩至 ~50 文件）、`ports/freertos/include/pj/config_site.h`（PJMEDIA 关闭外部依赖宏）、`ports/freertos/src/codec_stub.c`（只留视频桩）
> - 新增 `pjmedia/include/pjmedia-codec/config_auto.h`（空桩）
> - 修改 `FreeRTOS/application/pj_sip_dual_test.c`（RTCP 接入实时三线程）、`main.c`（接入全量自测）

---

## 目录

- [一、工作总览](#一工作总览)
- [二、Stage 13：精简 RTCP 引擎](#二stage-13精简-rtcp-引擎)
- [三、Stage 14：全量 pjmedia 框架接入](#三stage-14全量-pjmedia-框架接入)
- [四、pjproject 功能使用全景（重点）](#四pjproject-功能使用全景重点)
- [五、已测试功能可实现的应用](#五已测试功能可实现的应用)
- [六、未参与编译部分的应用场景](#六未参与编译部分的应用场景)
- [七、构建与运行](#七构建与运行)
- [八、余下的工作内容](#八余下的工作内容)

---

## 一、工作总览

| 阶段 | 内容 | 状态 |
|------|------|------|
| Stage 13 | 精简 RTCP 引擎（SR/RR/SDES + 丢包率/抖动/RTT），接入实时三线程通话 | ✅ 双向统计自洽 |
| Stage 14 | pjmedia 全套框架编入（endpoint/codec/G.711/stream/event/RTCP/会议/PLC…） | ✅ 全量自测 + 回归 PASS |

---

## 二、Stage 13：精简 RTCP 引擎

### 选型
评估 pjmedia 自带 `rtcp.c`：其 `parse_rtcp_fb()` 无条件依赖 `event.c` + `rtcp_fb.c`（后者又依赖 `codec.h/endpoint.h/vid_codec.h`），会把"反馈/NACK/事件订阅"整层拖进最小裁剪的 pjmedia；而 **XR 是可关的软依赖**（`PJMEDIA_HAS_RTCP_XR=0`）。测试场景只需要 SR/RR 统计，故自研 ~200 行精简引擎（RFC 3550 子集）。

### 实现（`pj_rtcp_engine.c/.h`）
- 报文：**SR**（PT=200，发送者统计 + 接收报告块 + SDES CNAME）、解析 RR/SDES/BYE
- 统计：发送 `pkt/octet`；接收 `expected/lost/fraction/interarrival jitter`（RFC A.8）；对端丢失报告（匹配本端 SSRC）
- **RTT = A - LSR - DLSR**（用系统 tick 作 NTP 参考时钟，自洽，1ms 量化）

### 接入（`pj_sip_dual_test.c`）
- RTCP 端口 = RTP+1（caller 4001 / callee 4003），hostfwd 同步加端口
- sender 每帧喂发送统计、每 500ms 发一次 SR + 结束发最终 SR；接收侧喂 `rx_rtp(seq, ts)`；主任务等待期非阻塞轮询 RTCP socket 解析
- **踩坑**：lwIP 多 fd `select`（RTP+RTCP 一起）让 RTP 接收崩溃（caller rx=2/200）→ 恢复 RTP 单 fd 路径，RTCP 由主任务独立轮询

### 验证（双 QEMU）
```
caller: rx exp=200 got=186 lost=14 frac=7% | peer-lost=0  | rtt=12000us
callee: rx exp=200 got=200 lost=0  frac=0% | peer-lost=14 | rtt=5000us
```
- caller 丢的 14 帧 ↔ callee 报告的 `peer-lost=14`（**双向自洽** ✓）
- 音频 439/1001Hz 完整，media ALL PASSED

---

## 三、Stage 14：全量 pjmedia 框架接入

### 动机
测试阶段把 pjmedia 全套功能携带上，评估**功能完整性 / 兼容性**，为将来接 pjsua 或更多能力打地基。

### 编入的源文件（~50 个）
- 媒体框架：`endpoint / codec / g711 / stream / stream_common / session / port / master_port / null_port / transport_udp / transport_loop`
- 控制/统计：`rtcp / rtcp_fb / rtcp_xr / event / errno / format / stream_info`
- 音频处理：`conference / conf_switch / splitcomb / bidirectional / echo_common / echo_port / echo_suppress / plc_common / wsola / resample_port / resample_resample / silence_det / tonegen / clock_thread / delaybuf / mem_capture / mem_player / wav_player / wav_writer / wave / stereo_port / audiodev`

### 关键配置与处理
| 项 | 处理 |
|---|---|
| 外部依赖全关 | `config_site.h`：`PJMEDIA_HAS_VIDEO/SRTP/FFMPEG/LIBYUV/SPEEX_AEC/WEBRTC_AEC(AEC3)=0`、`PJMEDIA_RESAMPLE_IMP=NONE`、仅 `G711_CODEC=1`（pjmedia/config.h 全 `#ifndef`，站点宏优先生效） |
| `pjmedia_aud_subsys_init` 未定义 | `pjmedia_endpt_create` 是 inline 且无条件调用它（实现在未编的 pjmedia-audiodev 库）→ 新增 `audiodev_stub.c` 空实现 |
| `close` 被 lwIP 宏替换 `lwip_close` | codec 的 `op->close` 成员被错误展开 → `#undef close` |
| `sin()` 未定义 | 项目禁浮点 → 自测用固定点正弦查表（无 libm） |
| codec API | `find_codecs_by_id` 第 4 参是**指针数组** + 第 5 参 `NULL`；编解码走 `codec->op->open/encode/decode` |
| codec_stub | 5 个 audio 桩移除（真 `codec.c` 接管），只留视频桩 |

### 全量自测（`pj_media_full_test.c`，非 dual 路径）
```
endpt create OK | codec mgr + G.711 registered | found PCMU/8000/1
PCMU encode=160B decode=160B peak=15996 (amp 16000)   ← 真 codec 端到端编解码
event mgr create OK | rtcp tx_pkt=100 rx_pkt=100 loss=0
pjmedia_full: ALL PASSED
```
且后续 `pj_sip_inv` / `pj_rtp` / `pj_call` 全部不变 —— **零功能退化**。

### 双 QEMU 通话回归
与精简版行为完全一致：`lost=14 ↔ peer-lost=14`、RTT 8-12ms、音频 439/1001Hz 全段有声、media ALL PASSED。

---

## 四、pjproject 功能使用全景（重点）

> 下表按库盘点当前 `ports/freertos/CMakeLists.txt` 的编译情况，分三类：**① 已编译且已测试**、**② 已编译但测试未实际调用**、**③ 未参与编译**。

### 4.1 已编译 + 已测试（实际跑通）

| 库 | 已测试功能 | 说明 |
|----|-----------|------|
| **pjlib** | 池/线程/互斥/信号量/原子/事件/定时器堆/日志/时间戳 | `pj_test` PASS |
| **pjlib**（网络） | socket、ioqueue 异步收发、activesock、DNS 解析、`pj_sock_select` | `pj_net_test` PASS（10.0.2.15 回环） |
| **pjsip** | 事务层、UA 层、dialog、SIP 解析/构造、UDP transport | REGISTER + INVITE 信令 |
| **pjsip-ua** | `sip_reg`（REGISTER 客户端）、`sip_inv`（INVITE 会话）、100rel、timer、replaces/xfer/siprec 头解析 | `pj_sip_test` REGISTER 200 OK |
| **pjmedia** | `rtp`（打包/解包）、`alaw_ulaw_table`（G.711 表）、`jbuf`（抖动缓冲）、**真 codec mgr + g711**、`rtcp`、`event`、`endpoint` | 全量自测 + 通话 |
| **pjlib-util** | md5/sha1/hmac、base64、xml、scanner、dns/srv resolver | pjsip 内部使用（URI/鉴权） |

**组合出来的能力**：SIP 软电话（REGISTER + INVITE + SDP 协商）→ 实时双向语音（G.711 + RTP + 三线程实时媒体 + jitter buffer）→ 质量监控（RTCP 丢包率/抖动/RTT）。

### 4.2 已编译，但测试尚未实际调用（可编译可用，验证面未覆盖）

| 模块 | 能力 | 未测原因 |
|------|------|----------|
| `conference / conf_switch / splitcomb` | 会议桥（多方混音） | 目前只做 1:1 通话 |
| `echo_common/echo_port/echo_suppress` | 基础回声抑制 | 未启用（外部 AEC 关闭） |
| `plc_common / wsola` | 丢包隐藏 PLC（WSOLA 变速补偿） | 通话已靠 jbuf 平滑，未启用 PLC |
| `resample_port/resample_resample` | 重采样端口 | `RESAMPLE_IMP=NONE`（无后端） |
| `wav_player/wav_writer/wave` | WAV 文件端口 | 测试用硬件 mic/audio，未走文件端口 |
| `delaybuf / tonegen / mem_capture / mem_player / stereo_port / silencedet` | 时延缓冲/DTMF 音调发生/内存端口/静音检测 | 未接入应用 |
| `transport_loop` | 回环传输 | pjsip 用 `sip_transport_loop`；media 层未用 |
| `codec mgr 的 L16` | L16 无损 PCM codec | 默认只注册 G.711 |

### 4.3 未参与编译（未编入，需外部库或按需拉入）

| 组件 | 内容 |
|------|------|
| **pjnath** | STUN / TURN / ICE（NAT 穿透整套） |
| **pjmedia-audiodev** | 音频设备抽象库（`sound_port` 等）→ 用 `audiodev_stub` 占位 |
| **pjmedia-videodev + vid_\*** | 视频设备/端口/流（`vid_port/vid_stream/vid_conf/videodev`…） |
| **transport_srtp\*** | SRTP 加密 RTP（SDES/DTLS） |
| **transport_ice** | ICE 传输 |
| **pjmedia-codec 库** | 外部 codec：opus / speex / gsm / ilbc / g722 / g7221 / l16 / openh264 / vpx / silk…（需第三方库） |
| **echo_speex / echo_webrtc(AEC3)** | 外部 AEC 引擎（speexdsp / libwebrtc，含 C++ 编译单元） |
| **resample_libsamplerate / resample_speex** | 外部重采样后端 |
| **converter_lib\*** | libyuv / libswscale 色彩空间转换 |
| **ffmpeg_util / ai_port\*** | FFmpeg 集成 / OpenAI 语音端口 |
| **pjsua-lib / pjsua2** | 高级 API 封装（软电话 App 层） |
| **pjlib SSL** | `ssl_sock_*`（TLS）→ pjsip 的 `sip_transport_tls` |
| **avi_player/avi_writer** | AVI 文件播放/录制 |
| **txt_stream** | T.140 实时文本（RTP 承载文字） |

---

## 五、已测试功能可实现的应用

基于已编译 + 已测试的能力，当前固件**已经是一个能跑通全流程的 SIP 软电话内核**：

1. **SIP 软电话（语音通话）**：REGISTER 注册 → INVITE 拨号/应答 → SDP 协商 → G.711 语音 → 双工通话。已实现：单实例自环、**双 QEMU 实例真实互拨**。
2. **实时通话引擎**：三线程（发送/接收/播放）10ms 实时节拍 + jitter buffer 抖动吸收 + 预填/优先级调优 —— 接近真实 VoIP 接收端。
3. **通话质量监控**：RTCP SR/RR 周期上报，得到丢包率、到达间隔抖动、RTT —— 可支撑"通话质量仪表盘/告警"。
4. **嵌入式 RTOS + 网络基座**：pjlib 线程/定时器/池 + lwIP socket/ioqueue —— 任何需要联网的嵌入式应用可复用。
5. **SIP 注册/在线能力**（pjsip-simple 已编）：MWI（留言提示）、presence（在线状态）、publish（发布状态）等 SIMPLE 扩展**已在库中**，稍作接入即可用。

---

## 六、未参与编译部分的应用场景

| 未编译组件 | 主要应用场景 |
|-----------|--------------|
| **SRTP** | 加密语音/信令 —— 企业保密通话、合规要求（金融/医疗） |
| **pjnath（STUN/TURN/ICE）** | NAT 穿透、P2P 直连 —— 公网通话/视频会议**无需服务器中转**；TURN 作为中继兜底 |
| **视频（vid\*/videodev + 视频 codec）** | 视频通话、远程监控、视频会议 |
| **外部音频 codec（opus/speex/gsm/ilbc/amr…）** | 高音质（opus 48k）、低带宽/弱网（amr/ilbc/gsm）、与传统网关互通（g722 等） |
| **AEC（speex/webrtc）** | 免提、车载、扬声器外放时的**回声消除**（当前用基础 suppress） |
| **TLS（pjlib ssl + sip_transport_tls）** | 加密 SIP 信令（sips:），防窃听 |
| **pjsua / pjsua2** | 快速搭建完整软电话 App（高层 API：账号/呼叫/会议/媒体管理），减少手写集成 |
| **重采样外部后端** | 多采样率互通（8k/16k/48k 设备与不同 codec 转换） |
| **实时文本（txt_stream）** | 无障碍/紧急通信的 T.140 实时文本 |

---

## 七、构建与运行

```powershell
# 单实例（含 pjmedia_full 自测 + 全链路 loopback）
cmake -B build -S . -G Ninja -DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/arm-none-eabi-gcc.cmake -DBOARD=mps2-an505 -DPROJECT=FreeRTOS
cmake --build build

# 双 QEMU 通话（含 RTCP）
cmake -B build-caller -S . ... -DPJ_DUAL_ROLE=caller   ; cmake --build build-caller
cmake -B build-callee -S . ... -DPJ_DUAL_ROLE=callee   ; cmake --build build-callee
powershell -ExecutionPolicy Bypass -File works\tools\run_dual_call.ps1
```
新增 `.c` 后需重新 `cmake -B`（file(GLOB) 缓存）；tools 见 `works/tools/`（make_sine_wav / analyze_call_audio / run_dual_call）。

---

## 八、余下的工作内容

- [ ] 应用层使用已编译的 **conference/PLC/AEC** 增强通话（会议桥、丢包隐藏、回声抑制）
- [ ] **DTMF**（RFC 2833 带内事件；tonegen 已可生成）
- [ ] **DWT 高精度时间戳**（改善 RTCP RTT 测量精度，当前 1ms tick 量化）
- [ ] 评估接入 **pjsua-lib**（高层 API，验证完整软电话）
- [ ] 按需拉入 **外部 codec（opus）** 或 **AEC（speex）** 评估多码率/免提
- [ ] 真实芯片（非 QEMU）时序与中断验证

---
*按 `works/` 目录约定整理；工具脚本见 `works/tools/`。*
