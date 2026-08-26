# WORKLOG 2026-08-26：电话应用工业化（可靠性加固 / 运行时换域 / 代码规范 / DTMF）

> 平台：QEMU mps2-an505 + Cortex-M33 + FreeRTOS + LVGL 9.5 + PJSUA(pjproject 2.17) + FreeSWITCH
> 对端：真实 Android 手机（Linphone，分机 1005）
> 今日主线：把"玩具级"电话往"工业级"推进 —— A(可靠性) → A+(运行时换域) → 代码规范 → B(DTMF，未完成)

---

## 1. 今日工作总览

| 任务 | 状态 | 说明 |
|---|---|---|
| 工业级差距规划 | ✅ | 8 大类工作 + 路线图（里程碑 0-4） |
| A 可靠性/错误处理加固 | ✅ | 作业一致性/回调失败/断流/热修复/init 清理 |
| A+ 运行时换域（UI 设置） | ✅ | 触摸屏改 dial host + 重新注册，无文件系统 |
| 代码风格统一 | ✅ | 4 个文件重排 + 6 条约定入记忆 |
| B DTMF 收发 | ⚠️ 未完成 | 发送/接收代码完成，但 guest→手机方向手机不显示，问题排查中 |

---

## 2. 工业级差距规划（M0-M4）

把"玩具级→工业级"拆成 8 大类：可靠性、安全、音频质量、功能完整性、工程化、网络部署、合规认证。
**建议顺序**：M0 基线固化(自动化回归+soak) → M1 可靠&安全骨架(TLS/SRTP/凭据加密) → M2 音频与功能(G.722/AGC/DTMF/hold/transfer/电话本) → M3 工程化(配置系统/日志/OTA/CI) → M4 打磨合规。
**今日从 A（可靠性）开始**。

---

## 3. A 阶段：可靠性/错误处理加固（已完成）

### 3.1 作业投递状态一致性（防 UI 永久卡死）
- `phone_job_post`：`pjsip_endpt_schedule_timer` 失败必须复位 `g_job`（否则作业槽永久 EBUSY）
- `pj_phone_dial` post 失败 → 回滚乐观 DIALING→IDLE
- `phone_job_exec`：make_call 失败 → IDLE；answer/reject 失败 → IDLE
- 统一辅助函数 `phone_to_idle()`（清双 id + 状态 + notify）

### 3.2 回调/初始化错误处理
- busy-486 应答失败、`pjsua_set_ec`、`pjsua_conf_connect` 失败均检测打印
- init 中 `pjsua_set_snd_dev(mpsx)` 失败 → fallback null 设备（信令仍可用）
- init 失败路径（pjsua_init/transport/start/acc_add）全加 `pjsua_destroy()` 清理

### 3.3 通话结束原因分类
- DISCONNECTED 记录 `g_last_call_status`（SIP code: 486/408/480/487/200...）+ text
- 新 API：`pj_phone_get_last_call_status()` / `_text()`
- UI：异常结束后显示 `Ended: <reason>` 3 秒

### 3.4 媒体断流自动处理
- watchdog 检测"我方仍在发 RTP、rx 冻结" → `g_media_stall` 标记
- UI 显示 `No audio! mm:ss`，超过 `PJ_PHONE_MEDIA_STALL_HANGUP_MS`（默认 60s，可配 0=仅提示）自动挂断
- RX 恢复自动清除

### 3.5 注册域热修正
- 新 API `pj_phone_reregister()`：worker 线程 `pjsua_acc_get_config` → 改 `ac.id` → `pjsua_acc_modify` → `pjsua_acc_set_registration(TRUE)`
- 宿主 IP 漂移后 `pj_phone_set_dial_host()` + `reregister()` 无需重编

### 3.6 环境坑：宿主 DHCP IP 漂移 → 注册 403
- 现象：注册 `403 Forbidden`，FS 日志 `Can't find user [1000@旧IP]`
- 根因：宿主 DHCP 把 LAN IP 从 `.6` 换成 `.7`；FS `domain=$${local_ip_v4}`，目录只认新域
- 修复：更新 `PJ_PHONE_DIAL_HOST` 默认值 + 注释说明（根治方向=持久化配置）

---

## 4. A+ 阶段：运行时换域（UI 设置，已完成）

- **Host 设置模式**：IDLE 按 `#` 进入（`s_setting_host`），拨号盘编辑 IP（`*`=小数点），`C`=SAVE（`pj_phone_set_dial_host` + `reregister`），`#`=取消
- 新增 `pj_phone_get_dial_host()`（UI 预填当前值）
- 状态行显示 `SET HOST (dot=*, #=exit)`，保存后 `Host set!` 3 秒
- **littlefs 持久化暂缓**（用户要求）：PJ_PHONE 跳过 fatfs 自测、256MiB littlefs 首次 format 慢，先不碰文件系统

---

## 5. 代码风格统一（已完成，入记忆 code-style.md）

对 `pj_phone.c/.h`、`lv_phone_app.c/.h` 重排，约定：
1. 注释简洁英文、单独行、无行尾注释
2. 函数大括号同行 `int foo(void) {`
3. if 一律大括号 `if (...) {`
4. 局部变量尽量初始化
5. 类型与名字单空格
6. 头文件函数声明之间不留空行
7. 行尾 CRLF

---

## 6. B 阶段：DTMF 收发（完成代码，但 guest→手机方向未通 —— 重点）

### 6.1 已实现
- 发送：`pj_phone_send_dtmf(digits)`（仅 ACTIVE，直接调 `pjsua_call_dial_dtmf`，不走 job 单槽防丢音）
- 接收：`cfg.cb.on_dtmf_digit` 回调 + 移位缓冲 `g_rx_dtmf[16]` + `pj_phone_get_rx_dtmf()`
- UI：通话中按键发 DTMF（不再追加号码）；号码行显示 `RX: <dtmf>`

### 6.2 问题：guest→手机 DTMF 手机不显示（不对称）
**现象**：
```
1. guest 给手机拨打电话，手机主动挂断，guest 感知不到（已修：出站代理，昨日）
2. 手机→guest 方向 DTMF：guest 能收到（DTMF rx '2'...）✅
3. guest→手机 方向 DTMF：手机不显示 ❌（不对称）
```

**排查链路（全链路实测）**：
| 环节 | 结果 | 证据 |
|---|---|---|
| guest UI 触发 | ✅ | `send_dtmf("1") state=3` |
| guest pjsua 入队 | ✅ | `pjsua_call_dial_dtmf -> 0` |
| guest media 发送 | ✅ | `strm... Sending DTMF digit id 1`（stream.c create_dtmf_payload） |
| guest transport 发送 | ✅ | `pjmedia_transport_send_rtp` 无 "Error sending RTP" |
| FreeSWITCH 接收 | ✅ | `switch_channel.c RECV DTMF 1:1600` |
| 手机显示 | ❌ | 收不到/不识别 |

**根因 1（已修复）：payload 类型非标准**
- FS 日志实锤：guest 腿 `a=rtpmap:120 telephone-event/8000`，手机腿 `a=rtpmap:101 telephone-event/48000`
- 根因：本项目移植版 `pjmedia/include/pjmedia/config.h` 把 `PJMEDIA_RTP_PT_TELEPHONE_EVENTS` 默认改为 **120**（上游标准 101）
- 修复：`ports/freertos/include/pj/config_site.h` 覆盖 `#define PJMEDIA_RTP_PT_TELEPHONE_EVENTS 101`（config_site.h 先于 pjmedia 加载，`#ifndef` 生效，未改上游）
- 验证：guest 腿已变 `101@8000` ✅

**根因 2（新发现，疑似）：采样率不匹配 8000 vs 48000**
- 修复 1 后 guest 用 `101@8000`，手机仍协商 `101@48000`（宽带）
- 假设：FreeSWITCH 透传 RFC2833 事件时**保留原始采样率**（guest 的 8000），手机协商 48000 → 手机丢弃/不识别
- 手机→guest 方向能通，因为 guest（pjsua）解析 DTMF 事件不挑剔采样率

**尝试方案 B（SIP INFO，已回滚）**：
- 把 internal.xml `dtmf-type=info`（FS 用 SIP INFO 转发，绕过 RTP 采样率）
- 结果：手机仍不显示，**且引入新问题——guest 按 DTMF 次数多了手机自动挂断**（reason=200 Normal call clearing）
- 已回滚（`fs_disable_dtmf_info.ps1`）

**待验证方案 A：手机强制窄带 8k（PCMU only）**
- Android Linphone 设置 → 音视频 → 编码解码器 → 只保留 PCMU，禁用宽带 codec
- 若手机协商 8k 后 DTMF 显示 → 确认根因=采样率不匹配
- 若仍不显示 → 需抓包确认 FS→手机 是否有 DTMF 包（pktmon/Wireshark 或 uuid_debug_media）

### 6.3 DTMF 调试经验（方法论）
- 分层定位：UI → pjsua → media(stream) → transport → 网络 → FreeSWITCH → 手机，每层用日志确认
- FreeSWITCH 日志关键点：`RECV DTMF <digit>:<duration>`（确认 FS 收到）、`a=rtpmap:.*telephone-event`（对比两端 payload 类型和采样率）、`Set telephone-event payload to X@Y`
- 本移植版 `PJMEDIA_RTP_PT_TELEPHONE_EVENTS` 非标准(120)，`PJMEDIA_DTMF_DURATION=1600`(200ms) 正常

---

## 7. 未决问题 & 明日计划

### 未决
- **guest→手机 DTMF 手机不显示**（根因高度怀疑采样率 8000/48000 不匹配，待方案 A 验证）
- 若确认采样率 → 长期方案选项：
  1. guest 编宽带 codec（G.722 16k / Opus 48k）让两端采样率一致（工作量大）
  2. 让 FreeSWITCH 转换 DTMF 事件采样率（需查 FS 配置/补丁）
  3. 手机保持窄带（降质，不理想）

### 明日可做
- 验证方案 A（手机强制 PCMU 8k）→ 确认/排除采样率根因
- 若 A 不通 → 抓包（pktmon/Wireshark 或 uuid_debug_media）确认 FS→手机 是否有 DTMF 包
- 之后继续 C（自动化回归脚本）或 A+ 持久化配置

---

## 8. 今日产物清单

| 类型 | 内容 |
|---|---|
| 代码 | `pj_phone.c/.h`（A/A+ 加固 + DTMF + 换域）、`lv_phone_app.c/.h`（UI 设置模式 + DTMF 显示）、`config_site.h`（telephone-event=101） |
| 脚本 | `works/tools/fs_enable_dtmf_info.ps1`、`fs_disable_dtmf_info.ps1`（已回滚） |
| 记忆 | `/memories/repo/pjsua-build.md`（A/A+/DTMF 坑）、`/memories/repo/code-style.md`（风格约定） |
| 验证 | 各阶段冒烟均通过（注册 200 OK、无回归）；DTMF 手机侧待真机验证 |
