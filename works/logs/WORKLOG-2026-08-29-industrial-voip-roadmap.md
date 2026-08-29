# WORKLOG-2026-08-29：工业级 VoIP 电话 —— 差距清单与路线图（更新版）

> 平台：QEMU mps2-an505 + Cortex-M33 + FreeRTOS + LVGL 9.5 + PJSUA(pjproject 2.17) + FreeSWITCH
> 网络：**tap0 独立网段方案**（2026-08-29 定稿：Hyper-V 下网桥不可用，改用 OpenVPN TAP + 172.16.23.0/24）
> 对端：真实 Android 手机（Zoiper/Linphone，分机 1005）
> 本文件是 08-26 `industrial-phone.md` 的**更新总版**：补入 tap0 网络、DTMF 链路结论、120105 遗留、采样率根因。

---

## 0. 一句话现状

**"实验室能打电话"已经 100% 达成**（注册/双向 RTP/DTMF 收发/AEC 全通），
**"工业级可部署电话"还差：真实硬件、安全(SRTP/TLS)、音频质量(G.722/AGC)、功能(hold/transfer/会议)、工程化(配置持久化/OTA/CI)、NAT 穿透、合规认证。**

---

## 1. 当前已具备的能力（已实测）

| 能力 | 状态 | 备注 |
|---|---|---|
| SIP 注册/保活/重注册/换域 | ✅ | 注册 200 OK；`pj_phone_reregister()` 运行时换域 |
| 基本呼叫 | ✅ | 拨号/接听/拒接/挂断，双向 RTP，loss=0 |
| 双向媒体 | ✅ | G.711 PCMU/PCMA，RTCP rx/tx/loss 统计，20s+ 稳定 |
| DTMF 收发 | ✅(guest 侧) | RFC2833，guest 收/发均验证；**手机显示问题=客户端侧**（见 2.6） |
| AEC 回声消除 | ✅ | Speex AEC（QEMU 下收敛，-20~30dB） |
| 媒体断流检测 | ✅ | watchdog 检测 rx 冻结，可配置自动挂断 |
| 可靠性加固 | ✅ | 作业一致性/回调失败/init 清理/通话结束原因分类 |
| 高精度时间戳 | ✅ | SysTick 组合计数器，RTCP rtt 真实(3-5ms) |
| 网络环境 | ✅ | tap0 方案：guest 172.16.23.50 ↔ 宿主 172.16.23.1 ↔ FreeSWITCH，零 NAT 零补丁 |
| 电话 UI | ✅ | LVGL：拨号盘/通话界面/设置模式(换域)/DTMF 显示 |

---

## 2. 差距清单（8 大类，按优先级）

### 2.1 可靠性（🔴 最高优先级：先解决已知缺陷）
| 项目 | 现状 | 待办 |
|---|---|---|
| **RTP socket 不足（120105）** | ✅ **已解决（2026-08-29）**：非泄漏，是 `MEMP_NUM_UDP_PCB=8` 太小。实测每次通话占 ~4 个 UDP_PCB（RTP/RTCP 等多 socket）+ 系统 ~2（SIP/命令服务器）→ 第二次通话需要 10>8 → `socket()` ENOBUFS | 修复：`libutils/lwip/ports/lwipopts.h` 的 `MEMP_NUM_UDP_PCB 8→24`、`MEMP_NUM_NETCONN 12→24`；`phone_net.c` 加 `memp` 命令查看池用量；验证 3 次连续通话均正常，挂断后回落 2/24 无泄漏 |
| 长时间 soak 稳定性 | ⚠️ 未系统测 | 24h+ 连续通话/挂断循环回归 |
| **注册断线恢复** | ✅ **已完成（2026-08-29）** | watchdog 每 15s 主动注册保活（`reg keepalive probe` → `pjsua_acc_set_registration`）；FS 断线 → REGISTER 408 失败检测 → 自动重试；FS 恢复 → 200 OK 自动重连。`reg_timeout=90` 加快续期。已验证：profile stop→guest 408→start→guest 200 恢复。 |
| 自动回归 | ❌ | M0：脚本化冒烟 + soak 测试 |

### 2.2 安全（🔴 工业级硬门槛）
| 项目 | 现状 | 待办 |
|---|---|---|
| **SIP over TLS** | ❌ | pjsip TLS transport + 证书管理 |
| **SRTP 媒体加密** | ❌ | RFC 3711，pjsua `use_srtp` 配置 |
| 凭据安全 | ❌ | 分机密码明文（当前 1234 明文）；需加密存储 |
| 认证/防篡改 | ⚠️ | 目前仅 SIP digest |

### 2.3 音频质量（🟡 决定"听感"）
| 项目 | 现状 | 待办 |
|---|---|---|
| 宽带编解码器 | ❌ 仅 G.711 | 加 **G.722 (16k)** / **Opus (48k)**（顺带解决 DTMF 采样率不匹配根因） |
| **AGC 自动增益** | ❌ | pjsua `set_ec` 旁补 `set_agc` / 语音增益控制 |
| **NS 降噪** | ⚠️ | Speex 降噪组件接入 |
| PLC 丢包隐藏 | ⚠️ | jitter buffer 有 plc，需按工业场景调优 |
| VAD/CNG | ❌ 当前 `no_vad=TRUE` | 工业级需 VAD+CNG 省带宽（配合安静检测） |
| 采样率一致性 | ⚠️ | guest 8000 vs 手机 48000 是 DTMF/音质隐患，宽带 codec 统一 |

### 2.4 功能完整性（🟡 决定"可用性"）
| 项目 | 现状 | 待办 |
|---|---|---|
| **Hold / Resume** | ❌ | pjsua `call_set_hold` |
| **Transfer（盲转/咨询转/参会）** | ❌ | `call_xfer` / `call_xfer_replaces` |
| 三方会议 | ❌ | pjsua 会议桥 |
| 呼叫前转/免打扰 | ❌ | FS 侧 + 客户端触发 |
| reINVITE / session-timer | ❌ | RFC 4028 |
| 电话本/联系人 | ❌ | littlefs + UI |
| 通话记录 | ❌ | 本地记录 |

### 2.5 工程化（🟡 决定"可维护/可部署"）
| 项目 | 现状 | 待办 |
|---|---|---|
| 配置持久化 | ❌ littlefs 暂缓 | 账号/服务器/音频参数存 Flash |
| **OTA 固件升级** | ❌ | 分区 + 下载 + 校验 + 回滚 |
| 日志系统 | ⚠️ 串口 printf | 分级/循环缓冲/远程日志 |
| 崩溃恢复 | ⚠️ watchdog 简易 | 状态持久化、看门狗分级 |
| CI/自动化 | ❌ | 构建+回归+soak 流水线 |
| 内存管理 | ⚠️ | 120105 相关 + 长期运行 leak 检查 |

### 2.6 DTMF（部分完成，收尾）
- ✅ guest 收/发 DTMF（RFC2833）均验证；FS 转发正常（`Send middle/end packet` 铁证）
- ❌ **手机不显示** guest 发的 DTMF —— 已定位为**手机客户端显示层**问题（Zoiper/Linphone），非链路问题；若需在手机显示：核对客户端 DTMF 设置(RFC2833)，或用宽带 codec 统一采样率（root cause 2）
- 遗留：DTMF 采样率 8000/48000 不一致（guest 窄带 vs 手机宽带）—— 长期用宽带 codec 根治

### 2.7 网络部署（🟢 中后期）
| 项目 | 现状 | 待办 |
|---|---|---|
| **NAT 穿透** | ❌ tap0 是点对点 | STUN/TURN/ICE（pjsua 支持） |
| QoS 标记 | ❌ | RTP DSCP/TOS |
| 多网络切换 | ❌ | Wi-Fi/以太网切换、断线续接 |
| 服务器冗余 | ❌ | 多账号/多注册、故障切换 |

### 2.8 硬件移植与合规（🟢 最终目标）
| 项目 | 现状 | 待办 |
|---|---|---|
| **真实硬件移植** | ❌ 全在 QEMU | mps2-an505 真板 / 目标 MCU；真实音频 codec(I2S/DAC/ADC)、真实网络 |
| 实时性保障 | ❌ | 中断延迟、调度优先级、音频路径实时性 |
| 电源/低功耗 | ❌ | 待机、唤醒 |
| Flash 驱动 | ⚠️ | SPI Flash 已驱动，待应用（配置/OTA） |
| 合规认证 | ❌ | 当地法规、音频质量认证(MOS/PESQ) |

---

## 3. 建议路线图（M0-M4，更新）

| 里程碑 | 内容 | 关键产出 |
|---|---|---|
| **M0 基线固化** | 解决 120105 socket 泄漏；自动化回归脚本；24h soak | 可连续通话、可重复验证的基线 |
| **M1 可靠 + 安全骨架** | TLS/SRTP/凭据加密；NAT 穿透(STUN/ICE)；断线恢复 | 能在公网/复杂网络跑通的安全电话 |
| **M2 音频 + 功能** | G.722/Opus、AGC/NS；Hold/Transfer/会议；DTMF 收尾 | 音质达标、功能完整的电话 |
| **M3 工程化** | 配置持久化、OTA、日志、CI 流水线 | 可部署、可维护、可升级 |
| **M4 硬件移植 + 合规** | 真板移植、实时性、功耗、认证 | 可量产的工业级设备 |

---

## 4. 立即建议的下一步（最小集）

1. **✅ 120105 已修**：`MEMP_NUM_UDP_PCB 8→24`、`NETCONN 12→24`（非泄漏，池太小），已连续 3 次通话验证
2. **🔴 自动化回归**：`works/tools` 的 UDP 命令 + fs_cli 已有基础，脚本化"注册→拨号→DTMF→挂断"冒烟
3. **🟡 DTMF 收尾**：手机强制窄带验证采样率根因（方案 A），或接受"链路通、手机显示问题"
4. **🟡 宽带 codec（G.722）**：一举解决采样率不一致 + 提升音质，为后续打基础

---

## 5. 记录/记忆
- 本清单作为长期 roadmap：`works/logs/WORKLOG-2026-08-29-industrial-voip-roadmap.md`
- 已入记忆：`/memories/repo/tap0-network.md`（网络方案）、`/memories/repo/speex-aec.md`（AEC）、`/memories/repo/pjsua-build.md`（A/A+/DTMF 坑）
