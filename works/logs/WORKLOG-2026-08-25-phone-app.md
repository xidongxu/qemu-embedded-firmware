# WORKLOG 2026-08-25：触摸屏 SIP 电话应用（设计 → 实现 → 调试）

> 平台：QEMU mps2-an505 + Cortex-M33 + FreeRTOS + LVGL 9.5 + PJSUA(pjproject 2.17)
> 对端：真实 Android 手机（Linphone，分机 1005）+ 宿主 FreeSWITCH
> 产物：`build-phone/boards/mps2-an505/FreeRTOS/an505-qemu.elf`
> 源码：`boards/mps2-an505/FreeRTOS/application/{pj_phone.c,pj_phone.h,lv_phone_app.c,lv_phone_app.h,lv_disp.c,main.c}`

---

## 1. 需求与总体设计

### 1.1 目标

在 QEMU 嵌入式固件上，用触摸屏实现一部可用的 SIP 电话：

- 拨号（通过拨号盘输入分机号 → 呼叫）
- 接听 / 拒接（来电时 UI 提示，触摸选择）
- 挂断（通话中触摸挂断）
- 状态显示（注册状态、通话状态、通话时长、对方号码）

### 1.2 架构选型

| 层 | 选型 | 说明 |
|---|---|---|
| 信令栈 | PJSUA（pjsua-lib 高层 API） | 封装 REGISTER/INVITE/ACK/BYE，回调驱动 |
| 协议栈 | pjsip + pjmedia + pjnath + pjmedia-audiodev | 项目手工精简构建（见 `libutils/pjprojec/ports/freertos/CMakeLists.txt`） |
| 音视频后端 | mpsx audio/mic（QEMU 虚拟声卡） | `application/mpsx_dev.c`，走 `pjmedia_aud_register_factory` 运行时注册 |
| UI | LVGL 9.5（450×450，双缓冲，触摸屏） | 复用项目自带 `lv_disp.c` 显示/触摸基建 |
| 对端 | FreeSWITCH（宿主）+ Android 手机 1005 | 经 QEMU slirp/hostfwd 互联 |

### 1.3 分层设计

```
┌─────────────────────────────────────────────┐
│  LVGL UI 层   lv_phone_app.c                 │  拨号盘 / 状态栏 / 上下文按钮
│  只读 pj_phone 状态 + 调标准接口 + 置 dirty  │
├─────────────────────────────────────────────┤
│  标准电话接口层  pj_phone.c / pj_phone.h     │
│  init/dial/answer/reject/hangup + 状态查询    │  不泄漏 pjsua 类型
│  呼叫控制投递到 pjsua worker 线程执行         │
├─────────────────────────────────────────────┤
│  PJSUA 高层 API（pjsua_create/init/acc/call）│  REGISTER + INVITE + BYE + 媒体
└─────────────────────────────────────────────┘
```

**核心设计决策：**
1. **UI 与信令解耦**：`pj_phone.h` 只暴露纯 C 的"手机"语义接口，不泄漏任何 pjsua 类型，LVGL 层可直接使用。
2. **跨线程安全**：pjsua 回调跑在 pjsua worker 线程，UI 跑在 LVGL 任务；回调只置 `s_dirty` 标志，LVGL 在 `lv_phone_app_update()` 里统一刷新（详见 §3.2 死锁问题）。
3. **配置运行时可改**：拨号目标域用 `pj_phone_set_dial_host()`，主机 DHCP 换 IP 不用重编（默认宏 `PJ_PHONE_DIAL_HOST`）。

### 1.4 标准接口设计（pj_phone.h）

```c
typedef enum { PJ_PHONE_REG_UNREGISTERED, REGISTERING, REGISTERED, FAILED } pj_phone_reg_state_t;
typedef enum { PJ_PHONE_CALL_IDLE, DIALING, INCOMING, ACTIVE } pj_phone_call_state_t;
typedef void (*pj_phone_cb_t)(void *user_data);   /* 状态变化通知（worker 线程回调） */

int  pj_phone_init(void);
void pj_phone_set_callback(pj_phone_cb_t cb, void *user_data);
void pj_phone_set_dial_host(const char *host, unsigned port);
int  pj_phone_dial(const char *number);    /* "1005" 或完整 "sip:user@host:port" */
int  pj_phone_answer(void);
int  pj_phone_reject(void);
int  pj_phone_hangup(void);
pj_phone_reg_state_t  pj_phone_get_reg_state(void);
pj_phone_call_state_t pj_phone_get_call_state(void);
const char           *pj_phone_get_peer_number(void);
unsigned long         pj_phone_get_call_duration_ms(void);
```

**状态机（g_call_state）：**

```
IDLE ──dial()──▶ DIALING ──CONFIRMED──▶ ACTIVE ──DISCONNECTED──▶ IDLE
  ▲                 ▲                      │
  └─────────────────┴──hangup()/远程挂断────┘
IDLE ──on_incoming_call──▶ INCOMING ──answer()→ACTIVE / reject()/hangup()→IDLE
```

### 1.5 UI 布局设计（450×450）

```
[ 状态行 ]           注册状态 + 通话状态
[ 号码行 ]           已拨号码 / 对方号码 / 通话时长
[ 1 2 3 ]
[ 4 5 6 ]            3×4 拨号盘（触摸输入）
[ 7 8 9 ]
[ * 0 # ]
[ A ][ B ][ C ]      上下文动作按钮
   idle:    CLR | DEL | CALL
   incoming: REJ |  -  | ANS
   active:   -  |  -  | HANG
```

> 注：项目 LVGL 字体只有 Montserrat（无 CJK 字形），**UI 文案一律用 ASCII**（CLR/DEL/CALL/REJ/ANS/HANG/IDLE/RING/ACTIVE 等）。

---

## 2. 实现

### 2.1 标准接口层（pj_phone.c）

**关键宏 / 常量：**

```c
#define FS_HOST        "10.0.2.2"     /* slirp 网关 = 宿主 loopback，固定 */
#define HOST_SIP_PORT  5060           /* FreeSWITCH SIP UDP 端口 */
#define GUEST_SIP_PORT 15062          /* guest SIP 端口（hostfwd 原样映射） */
#define REG_USER       "1000"         /* guest 在 FreeSWITCH 上的分机 */
#define REG_PASSWORD   "1234"
#define PJ_PHONE_DIAL_HOST "192.168.23.6"  /* 手机注册的 LAN 域（运行时可改） */
```

**（1）`pj_phone_init()` 流程：**

1. `pjsua_create()` → `pjsua_config_default`（挂 4 个回调）→ `pjsua_init`
2. 注册 mpsx 音频工厂（`pjmedia_aud_register_factory`）→ `pjsua_set_snd_dev(mpsx, mpsx)`；无 mpsx 时 fallback `pjsua_set_null_snd_dev()`
3. 建 UDP transport：`tcfg.port = 15062`，**`tcfg.public_addr = "127.0.0.1"`**（让 Contact/Via 对宿主可达，见网络文档）
4. `pjsua_start()`
5. 加账户：`acc_cfg.id = sip:1000@<g_dial_host>`（**LAN 域**，与手机同域）、`reg_uri = sip:10.0.2.2:5060`（**loopback 腿**）、**出站代理 `acc_cfg.proxy[0] = sip:10.0.2.2:5060`**、`cred realm="*" digest 1234`、`rtp_cfg.public_addr = "127.0.0.1"`、注册重试 5s
6. `pjsua_acc_add(..., PJ_TRUE, &g_acc)`（PJ_TRUE = 立即发起 REGISTER）

**（2）呼叫控制投递机制（phone_job_*）：**

```c
// 枚举：DIAL / ANSWER / REJECT / HANGUP
// phone_job_post()：把作业+号码存进静态变量，pjsip_endpt_schedule_timer(1ms)
// phone_job_exec()：在 pjsua worker 线程回调里真正执行 pjsua_call_*()
```

> 为什么必须这样：LVGL 任务直接调 `pjsua_call_make_call` 会死锁/冻结（详见 §3.2）。

**（3）回调处理：**

| 回调 | 行为 |
|---|---|
| `on_reg_state` | 2xx→`REGISTERED`；非 0→`FAILED`（自动重试，仅计数）；0→`REGISTERING`；可选 `PJ_PHONE_AUTO_DIAL` 自动拨号（默认关） |
| `on_incoming_call` | 忙时 486；否则存 `g_incoming_call_id`/`g_call_id`，状态→`INCOMING`，记对方号码 |
| `on_call_state` | 带 `[sec.msec]` 时间戳打印；CALLING/EARLY/CONNECTING→`DIALING`；CONFIRMED→记 `g_call_start`→`ACTIVE`；DISCONNECTED→打印 `reason`+`dur`，清双 id |
| `on_call_media_state` | 媒体 ACTIVE 时 `pjsua_set_ec(200,0)`（Speex AEC）+ `pjsua_conf_connect(ci.conf_slot,0)` / `(0,ci.conf_slot)` |

**（4）媒体关键配置：**

```c
media_cfg.no_vad = PJ_TRUE;            /* 关 VAD，防止静音被抑制丢 RTP */
media_cfg.snd_auto_close_time = -1;    /* 关自动关闭，防止媒体时钟停 */
media_cfg.snd_use_sw_clock = PJ_FALSE; /* 用 mpsx 原生时钟，防 capdbuf 下溢静音 */
```

### 2.2 LVGL 电话 UI（lv_phone_app.c / .h）

- `lv_phone_app_create()`：建状态行/号码行/3×4 拨号盘/三个上下文按钮
- 拨号键事件：数字追加进 `s_number`；`CLR` 清空、`DEL` 退格、`CALL`→`pj_phone_dial(s_number)`
- 上下文按钮随状态切换标签：idle `[CLR][DEL][CALL]`、incoming `[REJ][-][ANS]`、active `[-][-][HANG]`
- `lv_phone_app_update()`：每帧调用；`s_dirty`（回调置位）或通话中（每秒刷时长）时刷新
- 通过 `pj_phone_set_callback(phone_notify_cb, NULL)` 挂通知回调（只置 `s_dirty`）

### 2.3 集成（lv_disp.c / main.c）

- `lv_disp.c`：`lv_task_entry` 开头 `pj_thread_register("lv_task", ...)`；把 `lv_demo_benchmark()` 换成 `lv_phone_app_create()`；主循环每帧 `lv_phone_app_update()`
- `main.c`：PJ_PHONE 分支 **启用 LVGL**（`lv_task_init()`）+ 跳过 `audio_test/mic_test`（会和通话音频冲突）+ 跳过 `fatfs_test`（慢）；等 `lwip_os_task_init()` 网络就绪后 `vTaskDelay(1000)` 再 `pj_phone_init()`；另建 `phone_watchdog` 任务（3s 打印 heap / 任务栈高水位 / RTP 统计 / conf 信号电平）

### 2.4 构建

```powershell
cmake -B build-phone -S . -G Ninja -DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe `
     -DCMAKE_TOOLCHAIN_FILE=C:/Users/xidon/code/github/qemu-embedded-firmware/cmake/arm-none-eabi-gcc.cmake `
     -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DPJ_PHONE=ON
cmake --build build-phone
```

> 坑：新增 `application/*.c` 后必须重新跑 configure（`file(GLOB)` 不自动刷新，Ninja 只监控 CMakeLists 变更）。

---

## 3. 调试历程（关键里程碑）

### 3.1 构建修复：`sip_autoconf.h: No such file or directory`

- **现象**：clean 后构建报找不到 `sip_autoconf.h` / `config_auto.h`。
- **根因**：手工精简构建不执行上游 CMakeLists 的 `configure_file()`，这些 gitignored 生成头在 fresh clone / `git clean` 后缺失。
- **解决**：在 `libutils/pjprojec/ports/freertos/CMakeLists.txt` 顶部补 `configure_file()`，生成到 `ports/freertos/include/{pjsip,pjmedia,pjmedia-codec}/`；`PJMEDIA_HAS_G711_CODEC=1`。
- **结果**：构建通过，elf 正常产出。

### 3.2 UI 冻结 / 死锁：外部任务调 pjsua

- **现象**：触摸 CALL 后 LVGL 任务 `st=RDY` 忙转，无 make_call 日志，UI 冻结。
- **排查**：先加 `pj_thread_register`（解决 `pj_thread_this()` 为 NULL → PJSUA_LOCK 死循环），**不够**。
- **真正根因**：pjsua 媒体传输是**异步初始化**，只有在其 worker 线程的事件循环上才能推进；LVGL 任务直接调 `pjsua_call_make_call` 时传输永远不就绪 → 忙等。
- **解决**：把呼叫控制打包成一次性 pjsip 定时器（`pjsip_endpt_schedule_timer` 1ms），回调跑在 worker 线程。该 pjlib 定时器回调签名是 `(pj_timer_heap_t*, pj_timer_entry*)`。
- **结果**：UI 秒级响应，呼叫正常建立。

### 3.3 慢响铃（~10s）

- **现象**：拨号后要 ~10s 才响铃。
- **两层根因**：
  1. guest 侧 `lwip_os_test` 后台 HTTP 洪泛 + ping 抢占 RX → 已用 `#if defined(PJ_DUAL_ROLE_CALLER)||defined(PJ_DUAL_ROLE_CALLEE)||defined(PJ_PHONE)` 跳过；
  2. **FreeSWITCH `default.xml` 的 "global" extension 在 `default_password=="1234"` 时对每通呼叫 `sleep 10000`** —— 这才是主因。
- **解决**：`works/tools/fs_remove_default_password_sleep.ps1`（管理员运行，把 `sleep 10000` → `sleep 0`，`fs_cli -x "reloadxml"`，不改密码）。
- **结果**：响铃时间 10s → 216ms（make_call [12.628] → EARLY [12.844]）。

### 3.4 来电（手机→guest）不通 + 手机挂断检测不到

- **现象**：手机拨 1000 报"不在服务中"；即使接通，手机挂断 guest 也感知不到。
- **根因**：域不匹配 —— guest 注册在 `internal-lo`（10.0.2.2 域），手机在 `internal`（192.168.23.6 域）；FreeSWITCH 拨号计划按**主叫域**用 `sofia_contact(1000@<lan-ip>)` 找被叫，在手机域找不到 1000 → "not in service"；BYE 也发不回来。
- **解决**：`acc_cfg.id = sip:1000@<g_dial_host>`（**LAN 域**），但 `reg_uri` 仍走 `10.0.2.2` **loopback 腿**（源=127.0.0.1 不在 `nat.auto` ACL → Contact `127.0.0.1:15062` 不被 NAT 重写）。
- **结果**：来电可接通、可接听、手机挂断能感知（详见问题文档 §4）。

### 3.5 拨出（guest→手机）手机挂断检测不到 —— 见《问题与解决》文档重点章节

### 3.6 音频回环 / 媒体

- mpsx 音频工厂运行时注册（不改上游）；`snd_use_sw_clock=PJ_FALSE` 修采集静音；Speex EC（`pjsua_set_ec(200,0)`）修回声收敛。

---

## 4. 当前状态与验证结果

| 场景 | 结果 |
|---|---|
| 注册 | `reg state=200 (OK)` |
| 拨号（touch） | 秒级响铃（~216ms） |
| 接听（touch ANS） | CONFIRMED + 媒体 ACTIVE + RTP 双向 |
| 拒接（touch REJ） | 486 |
| 挂断（touch HANG） | BYE 正常 |
| 手机 → guest 来电 | 可接听，手机挂断 guest 感知（`reason=200`） |
| guest → 手机拨出 | 可接通，手机挂断 guest 感知（`reason=200`） |
| 通话时长显示 | UI 实时刷新，DISCONNECTED 打印 dur |

全部三个方向问题（慢响铃 / 来电 / 拨出挂断）已于 2026-08-25 实测通过。

---

## 5. 运行方式（手动测试）

```powershell
qemu-system-arm.exe -machine mps2-an505 -cpu cortex-m33 -m 16M `
  -kernel .\an505-qemu.elf `
  -display sdl,show-cursor=on -serial stdio `
  -nic "user,id=n0,model=lan9118,mac=52:54:00:12:34:01,hostfwd=udp::15062-:15062,hostfwd=udp::4000-:4000,hostfwd=udp::4001-:4001"
```

- 需宿主 FreeSWITCH 运行（internal-lo + internal 两个 profile，见网络文档）
- Android 手机 Linphone 注册 1005 到 `192.168.23.6:5060`，密码 1234
- 相关脚本：`works/tools/fs_bind_all.ps1`、`fs_add_loopback_profile.ps1`、`fs_fix_loopback_domain.ps1`、`fs_remove_default_password_sleep.ps1`
