# 2026-08-19 工作记录：SIP 通话功能全景（RTCP / 全量 pjmedia / DTMF / DSP）+ pjproject 功能使用度评估

> 本文档整理 2026-08-19 当天完成的工作：在既有 PJSIP 通话链路上补齐 **RTCP 质量监控（Stage 13）→ 全量 pjmedia 框架（Stage 14）→ DTMF 拨号（Stage 15）→ DSP 处理（PLC/AEC/会议，Stage 16）**，并给出 **pjproject 功能使用度百分比估算**、已用/未用功能清单，以及后续工作规划。
>
> 对应代码：本次变更**尚未提交**。新增：`pj_rtcp_engine.c/.h`、`pj_media_full_test.c/.h`、`pj_media_dsp_test.c/.h`、`ports/freertos/src/audiodev_stub.c`、`pjmedia/include/pjmedia-codec/config_auto.h`；修改：`ports/freertos/CMakeLists.txt`、`config_site.h`、`codec_stub.c`、`pj_sip_dual_test.c`、`main.c`。

---

## 目录

- [一、今日工作总览](#一今日工作总览)
- [二、实现的功能清单](#二实现的功能清单)
- [三、Stage 16：DSP（PLC / AEC / conference）验证](#三stage-16dsp-plc--aec--conference验证)
- [四、pjproject 功能使用度百分比估算（重点）](#四pjproject-功能使用度百分比估算重点)
- [五、已使用功能清单](#五已使用功能清单)
- [六、未使用功能清单](#六未使用功能清单)
- [七、后续工作内容](#七后续工作内容)

---

## 一、今日工作总览

| 阶段 | 内容 | 验证 |
|------|------|------|
| Stage 13 | 精简 RTCP 引擎（SR/RR/SDES + 丢包率/抖动/RTT）接入实时通话 | 双 QEMU 双向统计自洽 |
| Stage 14 | pjmedia 全套框架编入（endpoint/codec/G.711/stream/event/RTCP/会议/PLC 等 ~50 文件） | 全量自测 PASS + 通话回归零退化 |
| Stage 15 | DTMF（RFC 4733 telephone-event）——caller 拨号、callee 识别 | 双 QEMU `rx="5#"` 端到端 |
| Stage 16 | DSP：PLC 接入通话 + AEC 回声消除 + conference 会议混音 | 单实例 DSP 自测 ALL PASSED + 通话回归 |

---

## 二、实现的功能清单

1. **RTCP 通话质量监控**：每 500ms 发 SR（含发送统计 + 接收报告块 + SDES CNAME），解析对端 RR，得到**丢包率 / 到达间隔抖动 / RTT**（RTT = A-LSR-DLSR，tick 参考时钟）。
2. **全量 pjmedia 框架**：endpoint / codec mgr / 真 G.711 编解码 / stream / port / transport / event / RTCP / conference / echo / PLC / WSOLA / WAV / 重采样框架全部编译链接；新增 `pj_media_full_test` 自测（endpt 生命周期 + PCMU 编解码 + event + RTCP 会话）。
3. **DTMF 拨号**：RFC 4733 telephone-event（PT=101，独立 seq/ts），通话中 caller 拨 "5#" 被 callee 端到端识别。
4. **DSP 处理链**：
   - **PLC**：丢失帧由 WSOLA 预测填补（替代静音），听感连续；
   - **AEC**：`echo_suppress` 后端纯回声消除 99%；
   - **Conference**：双端口 tonegen 混音到 master 输出。
5. **既有通话内核**（今日回归确认零退化）：SIP REGISTER/INVITE、实时三线程媒体（sender/rx/play）、G.711 + RTP + jitter buffer、双 QEMU 互拨、85% 通过标准。

---

## 三、Stage 16：DSP（PLC / AEC / conference）验证

### PLC（丢包隐藏）— 接入实时通话
`play_thread`：正常帧 `plc_save` 作参考；MISSING/ZERO 帧 `plc_generate` **预测填补替代静音**。
- 实测有丢包轮：`missing=14 plc_fill=14`（全部填补），peak 保持 15996。

### AEC（回声消除）
- 后端：`echo_suppress`（无外部 AEC 库时自动选择）。
- 纯回声场景（mic 捕获 = 播放到扬声器的远端信号）：**99% 消除**。
- ⚠️ 局限：对"回声 + 独立近端语音"混合会**过度抑制（语音也被消）**——基础抑制算法限制，完整 AEC（speex/webrtc）才能精确区分。

### Conference（会议混音）
- 两个 tonegen（697+1209Hz，DTMF "1" 双音）加入 `pjmedia_conf`，连到 master；驱动 master `get_frame` 100 次输出非零混音（`mixed-rms=150995634`）——多方混音正常。

### DSP 相关踩坑
| 坑 | 解决 |
|----|------|
| `pjmedia_echo_create` 是 7 参数**无 channel_count** | 多传参数会把 `samples_per_frame` 挤错 → 触发 `samples_per_frame>=10*rate/1000` 断言；按 `(pool,rate,spf,tail,latency,opt,&echo)` 调用 |
| `conference.c` 即使 `NO_DEVICE` 也**链接期**引用 `pjmedia_snd_port_*` | `audiodev_stub.c` 补 `PJ_ENOTSUP` 桩（运行时不会调用） |
| `tonegen_play(LOOP + on_msec=0)` 挂死 conf get_frame | 用固定 `on_msec`（如 1000ms） |
| `long` 累加溢出（80×16000²、百帧求和） | 改用 `long long` 累加 |

---

## 四、pjproject 功能使用度百分比估算（重点）

> 说明：pjproject 完整源码约 **300+ 个 .c 源文件**（含 pjlib/pjlib-util/pjsip/pjsip-simple/pjsip-ua/pjmedia/pjnath/pjmedia-codec/pjmedia-audiodev/pjmedia-videodev/pjsua-lib/pjsua2）。我们**编译了约 137 个**，但**实际调用/测试的功能**少于编译量。

### 按编译层面估算：约 45%

| 库 | 全量源文件(约) | 已编译 | 编译占比 | 实际使用 |
|----|--------|--------|---------|---------|
| pjlib | 50 | ~30（核心+socket/ioqueue+FreeRTOS 移植） | 60% | 90%（线程/池/定时器/日志/网络全用） |
| pjlib-util | 20 | 13 | 65% | 70%（加密摘要/xml/scanner/dns；http/sip 客户端未用） |
| pjsip | 25 | 23（除 TLS+wrap） | 92% | 85%（信令核心全用） |
| pjsip-simple | 14 | 14 | 100% | 20%（编译全，evsub/presence 功能未测试） |
| pjsip-ua | 10 | 7 | 70% | 60%（inv/reg 用；replaces/xfer/siprec 只编译） |
| pjmedia（核心） | 80 | ~50 | 63% | 40%（endpt/codec/g711/rtp/jbuf/rtcp/event/plc/echo/conf 用；stream/transport 未真正跑 pjmedia_stream） |
| pjnath | 30 | 0 | 0% | 0%（STUN/TURN/ICE 未引入） |
| pjmedia-codec | 40 | 0 | 0% | 0%（外部 codec 未引入） |
| pjmedia-audiodev/videodev | 40 | 0 | 0% | 0% |
| pjsua-lib / pjsua2 | 60 | 0 | 0% | 0% |

**综合结论**：
- **编译层面：约 45%**（把 pjproject 主要库的核心部分都编进来了）
- **实际测试调用的功能：约 30%**（真正跑通验证的 API/模块）
- **协议/媒体能力层面（相对"完整软电话"）：约 70%**（信令+媒体+DTMF+RTCP+PLC+AEC+会议已具备，缺 SRTP/ICE/视频/多 codec/高层 API）

---

## 五、已使用功能清单

**已编译 + 已测试（实际跑通）**：
- pjlib：池 / 线程 / 互斥 / 信号量 / 原子 / 事件 / 定时器堆 / 日志 / 时间戳；socket / ioqueue / activesock / DNS 解析 / select
- pjlib-util：md5 / sha1 / hmac / base64 / crc32 / xml / scanner / dns / srv resolver
- pjsip：事务层 / UA 层 / dialog / SIP 解析构造 / UDP 传输 / 鉴权框架
- pjsip-ua：REGISTER / INVITE 会话 / 100rel / session timer / SDP 协商
- pjsip-simple：（编译齐全，evsub 等作为依赖）
- pjmedia：endpoint / codec mgr / **G.711（PCMU）** / RTP 打包 / jitter buffer / **RTCP** / event / **PLC** / **echo_suppress AEC** / **conference** / tonegen / WAV / 固定点重采样框架 / transport_udp（编译）

**组合能力**：SIP 软电话（注册/拨号/接听）→ 实时双向语音（G.711+RTP+三线程+jbuf+PLC）→ 质量监控（RTCP）→ 按键拨号（DTMF）→ 音频处理（AEC/会议）。

---

## 六、未使用功能清单

| 类别 | 组件 | 说明 |
|------|------|------|
| **NAT 穿透** | pjnath（STUN/TURN/ICE） | 未引入（本地/内网场景不需要） |
| **加密** | SRTP（transport_srtp）、TLS（pjlib ssl + sip_transport_tls）、mbedtls | 未引入（无加解密需求；将来需要时拉 mbedtls） |
| **视频** | vid_* / pjmedia-videodev / 视频 codec | 未引入 |
| **外部 codec** | opus / speex / gsm / ilbc / g722 / amr / openh264… | 需第三方库，未引入 |
| **完整 AEC** | speex / webrtc AEC | 需外部库；当前用基础 echo_suppress |
| **高层 API** | pjsua-lib / pjsua2 | 未引入（自研应用层） |
| **媒体流引擎** | pjmedia stream / transport_udp 实际跑流 | 已编译但通话用自研 media_ep；`pjmedia_stream` 未真正驱动 |
| **SIMPLE 扩展** | presence / MWI / publish（pjsip-simple） | 已编译未测试 |
| **重采样后端** | libsamplerate / speex resample | 未引入（RESAMPLE_IMP=NONE） |
| **音视频设备抽象** | pjmedia-audiodev / videodev | 未引入（用自研 mpsx 驱动 + 桩） |
| **实时文本** | T.140（txt_stream） | 未引入 |

---

## 七、后续工作内容

- [ ] **DWT 高精度时间戳**：替换 1ms tick，提升 RTCP RTT / 抖动测量精度（当前 8-12ms 量化到 1ms）
- [ ] **pjmedia_stream 真正接入**：用 pjmedia 的 stream + transport_udp 替代自研 media_ep，让 SDP 协商→codec→RTP→jbuf 全链路走 pjmedia（更接近生产实现）
- [ ] **SRTP 加密语音**：引入 mbedtls + transport_srtp，验证加密通话
- [ ] **会议桥接入通话**：把已编译的 conference 用于多方通话（>2 端）
- [ ] **多码率 / 外部 codec**：按需引入 opus（高音质）或 speex AEC（免提回声消除）
- [ ] **真实 SIP server / 多实例**：对接标准 SIP 服务器，验证互操作
- [ ] **DSP 落地应用**：把 AEC/会议从"自测"接入实际通话流程
- [ ] **真实芯片（非 QEMU）** 时序与中断验证

---
*按 `works/` 目录约定整理；构建/运行命令见 `works/logs/WORKLOG-2026-08-17-pjsip-call.md` 与 `works/tools/`。*
