# WORKLOG 2026-08-23 — PJSUA 高层电话应用（呼叫建立成功）

## 目标
用 PJSUA 高层 API（pjsua_*）在固件（mps2-an505 / FreeRTOS）上实现电话应用，拨号到宿主 pjsua（Windows）。

## 完成
### 1. 固件 pjproject 精简构建补全（ports/freertos/CMakeLists.txt）
原精简版只有 pjlib/pjlib-util/pjsip/pjsip-simple/pjmedia/pjsip-ua。为启用 pjsua 高层补编：
- `pjnath`（pjsua_media.c 无条件引用 pj_ice_strans_*）
- `pjmedia-audiodev`（audiodev/null_dev/errno；config 开 PJMEDIA_HAS_AUDIODEV=1，关其它后端、开 null）
- `pjsua-lib`（10 个 .c）
- 依赖补丁：stun_simple_client.c+stun_simple.c（pjlib-util）、txt_stream.c+transport_ice.c+audio_codecs.c（pjmedia）、file_io_ansi.c（pjlib）、audiodev_stub.c 删 aud_subsys stub + 补 snd_port stub、pjmedia include 加 pjnath

### 2. PJSUA 高层在 FreeRTOS 上完整跑通
`pjsua_create/init → null 音频 → UDP transport :15062 → start → acc_add → make_call`
拨号 `sip:user@10.0.2.2:5060`（slirp 网关）。宿主 pjsua UAS（auto-answer）应答。

### 3. 关键调试：200 OK 卡死 → 根因 + 修复
**症状**：收到 200 OK 后 pjsua_0 死循环（st=RDY 占 CPU），不发 ACK、不进 CONFIRMED。
**A/B**：486 拒绝完全正常（tsx 层自动发 ACK）；200 卡死（inv 层 inv_send_ack 前 CONNECTING 回调触发）。
**根因**：某处理 200 的线程 `pj_thread_t` 的 TLS 槽未设置 → `pj_thread_this()` 返回 NULL →
`PJSUA_LOCK_IS_LOCKED()`（owner==this==NULL）误判 true → `PJSUA_RELEASE_LOCK()` 的
`while(owner==this)` **无限解锁死循环**（owner 永不归零）→ 死循环 + 栈被反复 give 破坏。
**修复**（os_core_freertos.c `pj_thread_this`）：TLS 槽为 NULL 时惰性补设为 find_current_thread() 匹配的 rec。
**验证**：多次稳定 `CALLING → CONNECTING → SDP 协商 → 媒体(PCMU sendrecv) ACTIVE → TX ACK → CONFIRMED`；
宿主侧 "Answering call 0: code=200" + 媒体 Active。

### 4. 媒体 RTP（双向配置）
- guest→host RTP：宿主 `--ip-addr=10.0.2.2`（宿主 SDP c= 对 guest 可达）→ 宿主收到（RX pt=0）
- host→guest RTP：guest `rtp_cfg.public_addr=127.0.0.1`（guest SDP c=）→ 宿主发 RTP 到 127.0.0.1:4000（hostfwd udp::4000-:4000）→ guest

### 5. A1：RTP 双向稳定收包（no_vad + snd_auto_close 修复）
- **问题**：guest `tx_pkt=32` 停发 → 根因是 guest 的 null 音频设备空闲 1 秒被自动关闭（`snd_idle_timer`，`pjsua_media_config.snd_auto_close_time` 默认 1）→ 媒体时钟停 → stream 无音频源停发
- **修复**（`pj_phone.c` 的 `media_cfg`）：
  - `no_vad = PJ_TRUE`：禁用 VAD，null 静音源不被抑制，stream 持续编码/发送
  - `snd_auto_close_time = -1`：禁用音频设备自动关闭（-1 = 永不自动关闭）
- **验证**（60s，`--auto-play` 干净基线，`--jb-max-size=120`）：
  - guest `rx_pkt` 142→2401 持续增长 → host→guest 稳定收包
  - guest `tx_pkt` 70→1273 持续增长 → guest→host 稳定发包
  - guest `rx_lost`=132（5.5%）稳定不失控
  - 宿主 `Jitter buffer empty`=0 → 宿主持续解码 guest RTP
- **附注**：`--auto-loop`（宿主回环远端音频到 TX）可使双向持续 60s，但宿主 jitter-empty ~900/次（auto-loop 处理干扰宿主 RX 时钟，非 guest 发包问题——auto-play 基线为 0）；`--auto-play` 为干净 A1 基线（宿主仅在 10s WAV 期间持续发，但已验证 host→guest 全程收包）

### 6. A2：接入真实音频（mpsx audio/mic 后端，替代 null）
- **目标**：PJSUA 用真实的 QEMU mpsx 声卡（audio 播放 + mic 采集）替代 null 音频，让电话真正有声音
- **新增 `application/mpsx_dev.c`**（pjmedia-audiodev 后端，`PJMEDIA_AUDIO_DEV_HAS_MPSX`）：
  - 参考 `null_dev.c` 实现 factory_op + stream_op（13 个回调）
  - 播放：mpsx-simple-audio（0x51002000/IRQ49）DMA 缓冲设为**一帧大小（320B S16 @8k/20ms）**，每 DONE 中断 → FreeRTOS play_task 调 `play_cb` 拿 PCM 写回缓冲
  - 采集：mpsx-simple-mic（0x51003000/IRQ50）同样一帧缓冲，每 DONE → cap_task 读帧调 `rec_cb`
  - ISR 钩子：`audio.c`/`mic.c` 的 `Interrupt49/50_Handler` 加弱函数 `audio_done_hook`/`mic_done_hook`，mpsx_dev 强实现给信号量
- **注册（不改上游源码）**：`pj_phone.c` 在 `pjsua_init()` 后调上游公开 API `pjmedia_aud_register_factory(&pjmedia_mpsx_audio_factory)` 运行时注册（`pjsua_init` 内部 `pjsua_aud_subsys_init → pjmedia_aud_subsys_init` 已初始化子系统，register_factory 只是把 mpsx 设备追加进设备列表）；**不修改 `audiodev.c`/`config_site.h`**（已还原）。`PJMEDIA_AUDIO_DEV_HAS_MPSX` 宏在 **board 层** `FreeRTOS/CMakeLists.txt` 定义（仅 mps2-an505 PJ_PHONE），`pj_phone.c` 用 `#if` 守卫 mpsx 段（fallback null）
- **启用 snd 路径**：把 `sound_port.c` 编入 pjmedia 库、删除 `audiodev_stub.c` 的 snd_port 桩（此前返回 `PJ_ENOTSUP`）
- **pj_phone.c**：
  - `pjsua_set_snd_dev(mpsx_dev_id, mpsx_dev_id)`（`pjmedia_aud_dev_lookup("mpsx",...)`）替代 `pjsua_set_null_snd_dev()`
  - `on_call_media_state` 加 `pjsua_conf_connect(ci.conf_slot,0)` + `(0,ci.conf_slot)`：媒体 ACTIVE 后把 call 接到声卡（否则 call 不进 conference → 播放/采集无内容）
  - `media_cfg.snd_use_sw_clock = PJ_FALSE`：用 mpsx native 时钟（默认软件时钟 clock_thread+delaybuf 与 mpsx DONE 不同步 → capdbuf Underflow → 采集静音）
  - wd 加 `pjsua_conf_get_signal_level` 打印 conf tx/rx 电平（运行监控）
- **main.c**：PJ_PHONE 下跳过 `audio_test()`/`mic_test()`（琶音/阻塞采集会与通话音频冲突）；**wd 栈 512→2048**（加 conf sig 后 512 溢出 → 系统卡死，栈溢出破坏调度）
- **验证**（native 时钟，`--auto-play`）：
  - `pjsua_set_snd_dev(mpsx dev=1) -> 0`（真实设备打开）
  - `mpsx playback started` / `mpsx capture started`（两个 FreeRTOS 任务运行）
  - **采集**：mic infile 1kHz WAV → mpsx cap 帧 zcr=40（≈1kHz）→ conf → **发送 tx=99**（有内容）
  - **播放**：guest 收到 host 1kHz → play_cb 帧 zcr=40（1kHz）→ conf rx=99（有内容）
  - RTP 双向稳定：rx_pkt 持续、tx_pkt 持续、rx_lost 小
- **⚠ 已修复（QEMU 后端播放 500Hz，2026-08-23）**：QEMU `-audiodev wav` 录 guest 播放 500Hz 的根因 = `qemu-embedded-platform` 的 `hw/audio/mpsx_simple_audio.c` 自定义宏 `AUDIO_FORMAT_S16 (1)` 与 QEMU `AudioFormat` 枚举冲突（QEMU 里 `AUDIO_FORMAT_S16=3`）→ `as.fmt=1` 被音频引擎当作 **S8（8bit）** → 采样数翻倍 → 1kHz 播成 500Hz。**修复**：设备格式宏改名 `MPSX_FMT_*`、`mpsx_audio_fmt()` 返回 QEMU 枚举（S16=3/U8=0）、reset/FORMAT 校验用 `MPSX_FMT_*`。**验证**：播放录制中位 **1000Hz**。**注意**：QEMU 已用 mingw64 环境重编（`qemu-configure` ninja，PATH 加 `usr/bin`(sh)+`mingw64/bin`；`touch build.ninja` 可避免全量 regen），exe 拷到 `qemu-build`。宿主 `--rec-file` 录 guest 为空：宿主 pjsua rec-file 连接未执行（宿主配置问题，非 guest——guest conf tx=99 证明在发）

## 改动文件
- `libutils/pjproject/ports/freertos/CMakeLists.txt`：补编 pjnath/pjmedia-audiodev/pjsua-lib + 依赖源
- `libutils/pjproject/ports/freertos/include/pj/config_site.h`：PJMEDIA_HAS_AUDIODEV=1、音频后端裁剪、PJ_THREAD_DEFAULT_STACK_SIZE 16K
- `libutils/pjproject/ports/freertos/src/audiodev_stub.c`：删 aud_subsys stub、补 snd_port stubs
- `libutils/pjproject/ports/freertos/src/os_core_freertos.c`：**pj_thread_this TLS 惰性修复（核心）**、find_current_thread 256 guard
- `libutils/pjproject/pjlib/src/pj/lock.c`：grp_lock_acquire 16 链表遍历 guard
- `libutils/pjproject/pjsip/include/pjsua-lib/pjsua_internal.h`：PJSUA_UNLOCK 防下溢、PJSUA_RELEASE_LOCK 32 上限
- `boards/mps2-an505/FreeRTOS/application/pj_phone.c`：PJSUA 电话应用 + watchdog + rtp_cfg.public_addr + media_cfg.no_vad + snd_auto_close_time=-1（A1）
- `boards/mps2-an505/FreeRTOS/application/main.c`：PJ_PHONE 分支（lwip 网络后）+ 栈溢出/malloc hooks
- `boards/mps2-an505/FreeRTOS/application/FreeRTOSConfig.h`：configCHECK_FOR_STACK_OVERFLOW=2
- `boards/mps2-an505/FreeRTOS/CMakeLists.txt`：PJ_PHONE 选项 + 链接 pjsua-lib 等
- `works/tools/run_phone_test.ps1`：PJSUA 电话运行脚本（-Answer/-UseTimer/-IpAddr 参数 + RTP hostfwd + --jb-max-size + --duration=60 + auto-loop 说明）

## 运行
```
cmake -B build-phone -S . -G Ninja -DCMAKE_MAKE_PROGRAM=<ninja> -DCMAKE_TOOLCHAIN_FILE=<tc> -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DPJ_PHONE=ON
cmake --build build-phone
powershell -File works\tools\run_phone_test.ps1 -WaitSec 45 -Answer 200 -UseTimer 0 -IpAddr 10.0.2.2
```
