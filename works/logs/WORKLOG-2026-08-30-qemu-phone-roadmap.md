# WORKLOG-2026-08-30：QEMU 电话功能完整性与系统稳定性路线图（执行版）

> 平台：QEMU mps2-an505 + Cortex-M33 + FreeRTOS + pjproject 2.17 + mbedtls 4.2 + lwIP 2.2 + littlefs
> 网络：tap0 直连（guest 172.16.23.50 ↔ host 172.16.23.1 ↔ FreeSWITCH 5060/5061）
> 对端：双 QEMU 互拨 / FreeSWITCH / 真实 Android（Zoiper/Linphone 分机 1005）
> 关联：本文是 `WORKLOG-2026-08-29-industrial-voip-roadmap.md` 的 **QEMU 聚焦执行版**
> 状态：勾选框 = 后续按此文档逐项执行并勾选

---

## 0. 目标与范围（重要）

- **首要目标：功能完整性 + 系统稳定性**，全部在 QEMU 上开发与验证。
- **硬件移植、功耗、实时性、合规认证（08-29 的 M4）→ 冻结后置**，本阶段不做。
- 范围：M1（TLS/SRTP/凭据加密）已达成保留；执行音频/功能补全（M2）、工程化（M3）与稳定性贯穿工程。

---

## 1. 当前基线（已达成，勿重复）

| 能力 | 状态 | 依据 |
|---|---|---|
| SIPS/TLS 信令加密 | ✅ | mbedtls 4.2，TLS transport :5061，CA 校验，REG 200 OK |
| SDES SRTP 媒体加密 | ✅ | `use_srtp = PJMEDIA_SRTP_MANDATORY` |
| 凭据加密 | ✅ | `pj_crypto.c` AES-128-CBC 运行时解密，二进制无明文 |
| SNTP 时间同步 | ✅ | `sntp_sync.c`，RTC epoch 1788089259 |
| Speex AEC | ✅ | 数字回音收敛 -20~30dB |
| 注册保活/断线恢复 | ✅ | 15s keepalive 重注册，FS 断→408→恢复→200 |
| 媒体停滞监视 | ✅ | watchdog rx 冻结检测 + 自动挂断 |
| DTMF 收发 | ✅ | RFC 2833 收发 + 显示缓冲 |
| 高精度时间戳 | ✅ | SysTick 组合计数器，RTCP rtt 3-5ms |
| 崩溃转储 | ✅ | fault-dump（cortex-m33） |

---

## 2. 阶段一：通话功能补全（功能完整性 · 核心）

### 2.1 宽带语音（决定音质与 DTMF 采样率根因）
- [x] G.722（16k）交叉编译进 `ports`（`PJMEDIA_HAS_G722_CODEC=1` + `resample=LIBRESAMPLE` + `third_party/resample`），注册 `prio -> 0`
- [x] **mpsx 设备可变采样率**：`mpsx_dev.c` 用 `param.clock_rate` 配置 QEMU 设备 `REG_SAMPLE_RATE`（QEMU 已原生支持 1000-192kHz，零改动）；媒体链路 `clock_rate=snd_clock_rate=16000`
- [x] SDP 协商验证：拨 FS echo(9196)，`audio stream #0: G722 (sendrecv)` + SDES SRTP（`AES_256_CM_HMAC_SHA1_80`）
- [x] **G.722 双向音频验证**：pcap 铁证——guest→FS 5408 包 / FS→guest 5393 包，持续 108s 零丢包（20ms 帧 ≈50包/s）。注：FS 残留状态曾致入站 ~1590 包停（RX-STALL），`hupall`+干净环境后稳定，非 G.722 问题
- [x] G.711 fallback：G.711 恢复 NORMAL 作为窄带备选（设备 16k 下由 pjsua 自动 resample 处理 8k codec）
- [x] **Opus 交叉编译 + 集成**（`PJMEDIA_HAS_OPUS_CODEC=1` + `libutils/opus` libopus 1.6.1 `OPUS_FIXED_POINT=ON` + `ports/include/opus/opus.h` 转发头）；注册 `opus/48000/2`，`PJMEDIA_CODEC_OPUS_DEFAULT_COMPLEXITY=0`
- [x] **SIP 信令大包修复（Opus 引入的关键）**：Opus SDP 使 INVITE 达 2118B，而 lwIP `TCP_SND_BUF=2048` 只发出 2048B（截断）→ FS 等 body 超时 RST（503）。改 `libutils/lwip/ports/lwipopts.h` `TCP_SND_BUF 2048→8192`、`MEMP_NUM_TCP_SEG 16→40` 后 INVITE 完整发送 → 呼叫建立（`100→200 OK→CONFIRMED`）
- [x] **Opus 48k 协商验证**：media clock 48k 下 FS 通道 `read=opus rate=48000`（SDES SRTP），`audio updated: stream #0: opus (sendrecv)`
- [x] **Opus 48k 媒体在 QEMU/TCG 不可行（平台限制，留真机）**：25MHz M33 下 48k Opus 编码（即使 complexity 0）占满 CPU → 串口/解码/播放线程饿死 → 媒体静音（wav 全 0）、卡 CONNECTING（无 wd）。且 media clock 16k 时 RFC 7587 的 48000 RTP 时钟与 16k 不匹配 → Opus 不 offer（FS 落 PCMA）。**QEMU 宽带默认 G.722 16k（稳定已验证）；Opus 48k 全频需真机**（真机把 `media_cfg.clock_rate` 改回 48000 即可）
- [x] **16k 全链路回归**：G.722 16k @ 16k clock 双向 RTP 零丢包（rx_pkt≈tx_pkt 持续增长）、conf sig 有信号、wav 有 1kHz 回放

### 2.2 语音质量处理
- [ ] 自适应 jitter buffer 按工业场景调优（jbuf 参数 + `tc netem` 突发/延迟回归）
- [ ] G.711 PLC 丢包隐藏验证（netem 丢包 1%-5% 下听感/信号指标）
- [ ] AGC 自动增益接入（pjsua 增益控制，替代手动音量）
- [ ] Speex 降噪（NS）组件接入（`pjmedia_dsp`）
- [ ] VAD/CNG 评估：当前 `no_vad=1`，正式链路确认开关策略与省带宽效果
- [ ] 本地提示音：回铃音/忙音/拨号音 tonegen 接线

### 2.3 呼叫控制功能
- [ ] Hold/Resume（`pjsua_call_set_hold`）
- [ ] 盲转 / 咨询转（`pjsua_call_xfer` / `xfer_replaces`）
- [ ] 三方会议（pjsua 会议桥，现有 conf 接线扩展）
- [ ] 静音（麦克风静音）
- [ ] 呼叫等待 / DND / 前转（FS 侧 + 客户端触发）
- [ ] reINVITE / session-timer（RFC 4028）

### 2.4 多账号 / 电话本 / 通话记录
- [ ] 多账号多线路并行注册（`pjsua_acc` 数组化，双账号同注册/来电）
- [ ] 电话本（littlefs 持久化 + LVGL UI）
- [ ] 重拨 / 最近通话记录（本地落盘，重启保留）
- [ ] MWI 消息等待指示

### 2.5 DTMF 收尾
- [ ] 本地拨号音生成、`#`/`*` 功能键接线
- [ ] RFC 4733 覆盖 + 与宽带 codec 联合回归（Android 显示问题随采样率统一复查）

---

## 3. 阶段二：设备管理与 OTA（工程化 · 产品化）

- [ ] **配置系统**：账号/服务器/IP/音频参数从宏硬编码改为 littlefs 持久化配置
  - [ ] 配置读写模块 + 出厂重置 + 备份恢复
  - [ ] 串口管理 CLI（查/改/存/重置）
  - [ ] 验证：改配置→重启生效→重置回默认
- [ ] **OTA 固件升级**
  - [ ] 双分区 A/B 引导（spi-flash，现有 littlefs/驱动复用）
  - [ ] HTTP(S) 拉取 → 校验（哈希/签名）→ 写入 → 切换
  - [ ] 验证：构建新旧固件，升级成功；断电中断回滚旧版
- [ ] **日志系统**：分级/循环缓冲 + syslog 上报 + 日志滚动落盘
- [ ] **崩溃自恢复**：fault-dump 接自动重启 + 崩溃日志留存上报
- [ ] 网络管理面：DHCP 续租 / IP 变化自动重注册

---

## 4. 阶段三：系统稳定性加固（稳定性 · 贯穿工程）

### 4.1 长稳 Soak
- [ ] 连续注册/通话/挂断循环 24h+（双 QEMU + FreeSWITCH 自动压测脚本）
- [ ] 每次通话后 UDP_PCB / 内存水位回落基线（防 `120105` 类问题复发）
- [ ] 统计脚本输出：掉线次数 / 失败率 / 挂起呼叫

### 4.2 内存与任务健康
- [ ] TLSF 堆水印监控 + 泄漏检测 + 碎片统计进日志
- [ ] FreeRTOS 任务栈溢出检测开启 + 高水位上报
- [ ] 长跑后 heap 碎片率 / 最大块报告

### 4.3 网络韧性（异常注入）
- [ ] `tc netem` 注入：丢包/乱序/延迟/断网
- [ ] 断网→自动重注册→恢复回到注册态（注册保活已具备，补异常覆盖）
- [ ] NAT keepalive / OPTIONS ping（真实 NAT 场景预演）
- [ ] 非法 SIP 报文 / 畸形 SDP / TLS 中断重协商 / SRTP 重放 模糊注入后自恢复

### 4.4 编译与静态质量
- [ ] `-Wall` 告警清零（当前基线）
- [ ] 静态分析（cppcheck，可选）
- [ ] CI：构建 + 冒烟 + soak 一键脚本

---

## 5. 阶段四：进阶（可选，按需）
- [ ] 证书轮换 / 吊销检查（OCSP stapling，QEMU 搭 CA 验证）
- [ ] 固件签名验签 / 管理面加密（HTTPS/WSS）
- [ ] STUN/ICE（搭辅助 NAT 场景验证）
- [ ] 与 Asterisk / IMS 软交换互通矩阵

---

## 6. 冻结区（后置，本期明确不做）
- 真实硬件移植（音频 codec I2S/DAC/ADC、真实网络、真板）
- 功耗 / 低功耗待机 / 唤醒
- 中断延迟与调度实时性调优
- 合规认证（FCC/CE/运营商入网）
- 备注：以上留待"功能稳定后再启动"（触发条件 = 阶段一~三全部勾选）

---

## 7. 贯穿原则
1. **功能完成一项 → 顺带跑一轮 soak**（不攒到最后统一测）。
2. 所有改动不修改三方库源码，统一走 `libutils/*/ports/`。
3. 新增源文件遵循 `code-style.md`（CRLF 无 BOM、注释格式）。
4. 每项任务完成需附带 QEMU 验证证据（日志/截图/脚本输出）再勾选。

---

## 8. 完成定义（DoD）
- 阶段一：宽带 codec 双向可协商可通话；hold/transfer/会议在双 QEMU 场景跑通；电话本/记录重启后仍在。
- 阶段二：配置可持久化、OTA 升级与回滚成功、崩溃自动重启并留日志。
- 阶段三：24h soak 零挂起；内存水位稳定不涨；netem 异常注入后自动恢复。

---

## 9. 下一步执行（建议顺序）
1. **2.1 宽带语音（G.722/Opus 交叉编译）** —— 一次性打通采样率与 DTMF 根因。
2. **2.3 呼叫控制（Hold/Transfer/会议）** —— 电话核心功能。
3. **3.1 配置系统** —— 后续所有功能都依赖可配置化。
4. 之后进入阶段三稳定性专项。
