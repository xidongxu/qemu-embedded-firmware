# WORKLOG 2026-08-24 — FreeSWITCH 对接 + 给手机打电话（摆脱 DHCP IP 依赖）

## 目标
- 让 guest（QEMU mps2-an505 + PJSUA 电话应用）经**固定地址 10.0.2.2** 对接宿主 FreeSWITCH，摆脱对宿主 DHCP 分配 LAN IP 的依赖。
- 注册到 FreeSWITCH（分机 1000），并能**拨打真实手机**（分机 1005，Android Linphone）。
- 实测：注册 200 OK、拨号后**手机真正响铃（EARLY）**、应答后媒体交换。

## 拓扑
```
QEMU guest (PJSUA, 分机1000)
   │  REGISTER 10.0.2.2:5060  (slirp 网关 = 宿主 loopback)
   │  DIAL     sip:1005@192.168.23.7:5060
   ▼
FreeSWITCH  internal-lo profile  127.0.0.1:5060  （guest 注册域 = 10.0.2.2）
            internal  profile    192.168.23.7:5060（手机 1005 注册域）
   ▼
Android phone 1005 @ 192.168.23.4:49570 (Linphone)
```
- guest SIP 绑定 0.0.0.0:15062（hostfwd udp::15062-:15062）；RTP hostfwd udp::4000/4001。
- guest SDP 媒体地址用 `rtp_cfg.public_addr=127.0.0.1`（宿主发 RTP 到 hostfwd 端口）。

## FreeSWITCH 侧配置（工具脚本，改 `C:\Program Files\FreeSWITCH\conf\...`，均有 .bak-时间戳 备份）
| 脚本 | 作用 |
|---|---|
| `works/tools/fs_bind_all.ps1` | internal.xml 的 sip-ip/rtp-ip 绑 0.0.0.0 → 10.0.2.2(loopback) 也能到达；重启 FreeSWITCH |
| `works/tools/fs_add_loopback_profile.ps1` | 新增 `sip_profiles/internal-lo.xml`：sip-ip=127.0.0.1:5060、rtp-ip=$${local_ip_v4} |
| `works/tools/fs_add_loopback_domain.ps1` | 早期方案：`<domain name="10.0.2.2" alias="true"/>`（**无效**，REGISTER 仍 "Can't find user"→403） |
| `works/tools/fs_fix_loopback_domain.ps1` | 替代：directory/default.xml 插入**完整 `<domain name="10.0.2.2">` 块**（含 default/*.xml 用户）→ 解决 403 |

> 顺序：`fs_bind_all.ps1` → `fs_add_loopback_profile.ps1` → `fs_fix_loopback_domain.ps1`（fs_add_loopback_domain 已废弃，可不用）。

## 固件改动（boards/mps2-an505/FreeRTOS/application/）
### `pj_phone.c`
- 新增 `FS_HOST "10.0.2.2"`（注册目标，摆脱 DHCP）；`REG_USER "1000"` / `REG_PASSWORD "1234"`。
- 账户由"本地无注册"改为**注册到 FreeSWITCH**：`acc_cfg.reg_uri = sip:10.0.2.2:5060`，credential realm=`*`（匹配 FS 动态 realm），`pjsua_acc_add(..., PJ_TRUE, ...)`。
- **拨号域坑（本次关键）**：`DIAL_TARGET` 不再复用 `FS_HOST`，新增
  `DIAL_HOST "192.168.23.7"` → `DIAL_TARGET "sip:1005@" DIAL_HOST ":5060"`。
  原因：手机注册在 **internal 域(192.168.23.7)**，不是 internal-lo 域(10.0.2.2)。
  若拨 `sip:1005@10.0.2.2:5060`，sofia_contact 在 10.0.2.2 域找不到手机联系人 → 落 voicemail → guest 直接 CONFIRMED 无 EARLY。
- **单次呼叫模式**：删除 `pj_phone_control()`（auto-hangup 15s + redial 5s），启动拨一次、保持到对端挂断（避免反复响铃骚扰手机）；`main.c` 对应删除 `pj_phone_control()` 调用。
- 保留：`media_cfg.no_vad=TRUE`、`snd_auto_close_time=-1`、`snd_use_sw_clock=FALSE`、mpsx 声卡注册/接线、`pjsua_set_ec(200)`（Speex AEC）。

### `pj_phone.h` / `main.c`
- 删 `pj_phone_control()` 声明/调用；PJ_PHONE 分支启动后主循环只 vTaskDelay。

## 实测结果（2026-08-24，`run_phone_fs_test.ps1`）
guest 日志：
```
pj_phone: acc 0 reg state=200 (OK)                       ← 经 10.0.2.2 注册成功
pj_phone: make_call(sip:1005@192.168.23.7:5060) -> 0
pj_phone: call 0 state=3 (EARLY)                         ← ✅ 手机响铃
pj_phone: call 0 state=5 (CONFIRMED) → state=6 (DISCONNECTED)
```
FreeSWITCH 日志（决定性证据）：
```
New Channel sofia/internal/1005@192.168.23.4:49570       ← originate 到手机
entering state [proceeding][180] → Ring-Ready → RINGING  ← 手机真正响铃
Play Ringback Tone [%(2000,4000,440,480)]                ← guest 收到回铃音
Originate Resulted in Success → CS_EXCHANGE_MEDIA        ← 应答后媒体交换
```
对比修复前（拨 10.0.2.2）：FS 直接 `bridge(loopback/app=voicemail:default ...)` → guest 无 EARLY 直接 CONFIRMED。

## 构建 / 运行 / 测试
```powershell
# 1) 构建（仅首次需 configure，之后直接 build）
cmake -B build-phone -S . -G Ninja -DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe `
    -DCMAKE_TOOLCHAIN_FILE=C:/Users/xidon/code/github/qemu-embedded-firmware/cmake/arm-none-eabi-gcc.cmake `
    -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DPJ_PHONE=ON
cmake --build build-phone

# 2) 前置：FreeSWITCH 运行中 + internal-lo 配置 + 手机 1005 已注册（fs_cli: show registrations）

# 3) 自动测试（起 guest → 等 40s → 停 → 汇总关键日志）
powershell -ExecutionPolicy Bypass -File works\tools\run_phone_fs_test.ps1 -WaitSec 40 -Tag fs

# 4) 手动跑 guest（看串口实时输出）
& 'C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe' `
    -machine mps2-an505 -cpu cortex-m33 -m 16M -display none -serial stdio `
    -nic "user,id=n0,model=lan9118,mac=52:54:00:12:34:01,hostfwd=udp::15062-:15062,hostfwd=udp::4000-:4000,hostfwd=udp::4001-:4001" `
    -global "mpsx-simple-mic.infile=C:\Users\xidon\code\github\qemu-embedded-platform\testcase\sine_1k_8k_10s.wav" `
    -kernel build-phone\boards\mps2-an505\FreeRTOS\an505-qemu.elf
```
**判断通话正常**：
- 注册：`acc 0 reg state=200 (OK)`。
- 响铃：guest `call 0 state=3 (EARLY)`（手机应弹来电）；FS 侧 `Ring-Ready`/`RINGING`。
- 通话中：`wd: rx_pkt/tx_pkt` 持续增长、`rx_lost` 小；`conf sig tx/rx` 非 0（真实音频）。
- 接听后：`state=5 (CONFIRMED)`；FS 侧 `CS_EXCHANGE_MEDIA`。
- 真实音频：guest `-audiodev dsound`（`-machine mps2-an505,audiodev=a0 -audiodev dsound,id=a0,out.frequency=8000,out.channels=1`）可用宿主麦/音箱通话；脚本 `-RealAudio` 分支即此模式。

## 改动文件（本次提交）
- `boards/mps2-an505/FreeRTOS/application/pj_phone.c`（FS_HOST/DIAL_HOST/注册/单次呼叫）
- `boards/mps2-an505/FreeRTOS/application/pj_phone.h`、`main.c`
- `.gitignore`（+works/logs/*.err、*.txt 忽略）
- `works/tools/fs_bind_all.ps1`、`fs_add_loopback_profile.ps1`、`fs_add_loopback_domain.ps1`（废弃可留）、`fs_fix_loopback_domain.ps1`、`run_phone_fs_test.ps1`
- 本文件 `WORKLOG-2026-08-24-freeswitch-phone.md`
