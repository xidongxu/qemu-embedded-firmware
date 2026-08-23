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
- 注意：RTP 在 QEMU 虚拟时钟下不稳定（宿主 Jitter buffer empty/PLC）——环境固有限制

## 改动文件
- `libutils/pjprojec/ports/freertos/CMakeLists.txt`：补编 pjnath/pjmedia-audiodev/pjsua-lib + 依赖源
- `libutils/pjprojec/ports/freertos/include/pj/config_site.h`：PJMEDIA_HAS_AUDIODEV=1、音频后端裁剪、PJ_THREAD_DEFAULT_STACK_SIZE 16K
- `libutils/pjprojec/ports/freertos/src/audiodev_stub.c`：删 aud_subsys stub、补 snd_port stubs
- `libutils/pjprojec/ports/freertos/src/os_core_freertos.c`：**pj_thread_this TLS 惰性修复（核心）**、find_current_thread 256 guard
- `libutils/pjprojec/pjlib/src/pj/lock.c`：grp_lock_acquire 16 链表遍历 guard
- `libutils/pjprojec/pjsip/include/pjsua-lib/pjsua_internal.h`：PJSUA_UNLOCK 防下溢、PJSUA_RELEASE_LOCK 32 上限
- `boards/mps2-an505/FreeRTOS/application/pj_phone.c`：PJSUA 电话应用 + watchdog + rtp_cfg.public_addr
- `boards/mps2-an505/FreeRTOS/application/main.c`：PJ_PHONE 分支（lwip 网络后）+ 栈溢出/malloc hooks
- `boards/mps2-an505/FreeRTOS/application/FreeRTOSConfig.h`：configCHECK_FOR_STACK_OVERFLOW=2
- `boards/mps2-an505/FreeRTOS/CMakeLists.txt`：PJ_PHONE 选项 + 链接 pjsua-lib 等
- `works/tools/run_phone_test.ps1`：PJSUA 电话运行脚本（-Answer/-UseTimer/-IpAddr 参数 + RTP hostfwd）

## 运行
```
cmake -B build-phone -S . -G Ninja -DCMAKE_MAKE_PROGRAM=<ninja> -DCMAKE_TOOLCHAIN_FILE=<tc> -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DPJ_PHONE=ON
cmake --build build-phone
powershell -File works\tools\run_phone_test.ps1 -WaitSec 45 -Answer 200 -UseTimer 0 -IpAddr 10.0.2.2
```
