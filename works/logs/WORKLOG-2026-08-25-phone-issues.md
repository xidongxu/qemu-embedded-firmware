# WORKLOG 2026-08-25：触摸屏 SIP 电话 — 问题与解决思路合集

> 本文档按"现象 → 排查 → 根因 → 解决 → 验证"整理今天（及本轮迭代）遇到的所有问题。
> **重点章节：§4 手机挂断监听不到（拨出方向）** —— 含完整路由原理分析。

---

## 目录

1. [构建期：autoconf 生成头缺失](#1-构建期autoconf-生成头缺失)
2. [运行期：UI 冻结 / 死锁（外部任务调 pjsua）](#2-运行期ui-冻结--死锁外部任务调-pjsua)
3. [运行期：慢响铃 ~10s](#3-运行期慢响铃-10s)
4. [⭐ 重点：手机挂断监听不到（两方向不对称）](#4-重点手机挂断监听不到两方向不对称)
5. [运行期：来电打不通（"不在服务中"）](#5-运行期来电打不通不在服务中)
6. [附：其他已记录的坑](#6-附其他已记录的坑)

---

## 1. 构建期：autoconf 生成头缺失

**现象**：fresh clone / `git clean` 后构建报：

```
sip_autoconf.h: No such file or directory
```

**排查**：这些头（`sip_autoconf.h`、`pjmedia/config_auto.h`、`pjmedia-codec/config_auto.h`）是上游 CMake 用 `configure_file()` 生成的，且被 gitignore。本项目 pjproject 是**手工精简构建**（`libutils/pjprojec/ports/freertos/CMakeLists.txt`），不执行上游 CMakeLists，所以这些头从未被生成。

**解决**：在 `ports/freertos/CMakeLists.txt` 顶部补 `configure_file()`，输出到 `ports/freertos/include/{pjsip,pjmedia,pjmedia-codec}/`，并设 `PJMEDIA_HAS_G711_CODEC=1`。

**验证**：clean 构建通过。若再报缺头，检查这三个生成文件是否存在。

---

## 2. 运行期：UI 冻结 / 死锁（外部任务调 pjsua）

**现象**：触摸 `CALL` 后 LVGL 任务 `st=RDY` 忙转，串口无 `make_call` 日志，UI 完全冻结；拨号永远不发出。

**排查**：
1. 先用 `pj_thread_register("lv_task", desc, NULL)` 注册 LVGL 任务 → 解决了 `pj_thread_this()` 返回 NULL 导致的 `PJSUA_LOCK` 死循环，**但不够**，UI 仍冻结。
2. 深挖发现：pjsua 的**媒体传输是异步初始化**——`pjsua_call_make_call` 内部要等 media transport 就绪（`PJSUA_CALL_MEDIA_TRANSPORT_INIT`），而这个就绪只在 **pjsua worker 线程的事件循环**里推进。从 LVGL 任务直接调用时，worker 线程没机会跑 transport 就绪逻辑 → `pj_status_t` 一直返回 pending → 忙等。

**根因**：跨线程调用 pjsua 呼叫控制 API，且调用方不是 worker 线程，导致异步媒体初始化无法完成。

**解决**：设计"投递到 worker 线程"机制：

```c
// 1. 存作业（类型 + 号码）到静态变量
// 2. pjsip_endpt_schedule_timer(endpt, &timer, {0,1ms})  -- 一次性 1ms 定时器
// 3. 回调 phone_job_exec() 在 worker 线程执行 pjsua_call_*()

typedef enum { PHONE_JOB_NONE, DIAL, ANSWER, REJECT, HANGUP } phone_job_type_t;
static volatile phone_job_type_t g_job;
static pj_timer_entry g_job_timer;
static void phone_job_exec(pj_timer_heap_t *th, pj_timer_entry *e);  // 在此调 pjsua
```

> 注意：此 pjlib 移植版的定时器回调签名是 `void (*)(pj_timer_heap_t*, pj_timer_entry*)`（不是 pjsip 常见的带 `pj_time_val` 参数签名）。

**验证**：UI 秒级响应，`make_call` 日志正常，呼叫建立；拨入/拨出/挂断全部由 UI 驱动正常。

---

## 3. 运行期：慢响铃 ~10s

**现象**：guest 拨号后，手机要等 ~10 秒才响铃（两个方向都有）。

**排查（两层根因）**：

**层 1 — guest 侧**：`lwip_os_test` 任务在后台跑 HTTP 65KB 下载 + ping，在 QEMU TCG 单核下把 guest RX 队列占满，SIP 的 180/BYE 被挤掉。
→ 加 `#if defined(PJ_DUAL_ROLE_CALLER) || defined(PJ_DUAL_ROLE_CALLEE) || defined(PJ_PHONE)` 跳过这些后台测试。

**层 2 — 宿主 FreeSWITCH（主因）**：`C:\Program Files\FreeSWITCH\conf\dialplan\default.xml` 的 **"global" extension** 里有一段安全提醒逻辑：只要 `default_password` 还是默认值 `"1234"`，**每通呼叫都先 `sleep 10000`（10 秒）**！`vars.xml` 没改默认密码就会触发。

**解决**：`works/tools/fs_remove_default_password_sleep.ps1`（管理员运行）：
- 备份 `default.xml`
- 把 `<action application="sleep" data="10000"/>` → `data="0"`
- `fs_cli -x "reloadxml"`
- **不改密码**，所以所有已注册分机（1000/1005）都不受影响

**验证**：响铃时间从 ~10s 降到 ~216ms（`make_call [12.628] → EARLY [12.844]`）。

---

## 4. ⭐ 重点：手机挂断监听不到（两方向不对称）

### 4.1 现象（用户原始反馈）

```
1. qemu 给手机拨打电话，接通后，手机主动挂断，qemu 这边是感知不到的
2. 手机给 qemu 拨打电话，接通后，手机主动挂断，qemu 这边可以正常感知
```

同一个"手机挂断"，**拨入方向正常、拨出方向感知不到**——典型的不对称。

### 4.2 排查：FreeSWITCH 日志实锤

SIP 里"对方挂断"= 对端（手机）向 FreeSWITCH 发 BYE，FreeSWITCH 再把 BYE 转发给 guest。看 FreeSWITCH 日志里 "Sending BYE" 的目标就能定位问题：

```
# 拨入方向（手机 → guest，正常）：
Sending BYE to sofia/internal-lo/1000@127.0.0.1:15062      ← 带端口，真实 Contact，可达 OK

# 拨出方向（guest → 手机，异常）：
Sending BYE to sofia/internal/1000@192.168.23.6            ← 不带端口，是 AOR 而非 Contact，不可达 X
```

关键差异：
- **带端口** = 打到真实 Contact（`127.0.0.1:15062`，经 hostfwd 可达 guest）
- **不带端口** = 只按 AOR（`1000@192.168.23.6`）路由，这个地址没有监听，BYE 石沉大海

### 4.3 根因分析（为什么拨出走错了 profile）

FreeSWITCH 有两个 sofia profile：

| Profile | SIP 绑定 | 用途 |
|---|---|---|
| `internal` | 0.0.0.0:5060（LAN `192.168.23.6`） | 手机 1005 注册在这里 |
| `internal-lo` | 127.0.0.1:5060（经 slirp = `10.0.2.2`） | QEMU guest 1000 注册在这里 |

**问题链**：
1. guest 的**注册**走 `internal-lo`（reg_uri = `sip:10.0.2.2:5060`）
2. 但 guest 的**拨出 INVITE** 直接把 Request-URI 发到 `sip:1005@192.168.23.6:5060` → 落到了 **`internal`** profile
3. 于是这条**拨出呼叫的 dialog 建在 `internal` profile 上**（与注册的 `internal-lo` 不一致）
4. 手机挂断 → BYE 到达 `internal` → FreeSWITCH 在该 profile 的注册表里找 1000 的真实 Contact
5. 但 1000 注册在 `internal-lo`，`internal` 里查不到 Contact → 只能退化为按 AOR `1000@192.168.23.6` 发（无端口）→ 不可达

> 一句话：**拨出 dialog 与注册不在同一个 profile，BYE 找不到真实的 Contact 可发。**

### 4.4 解决：出站代理（outbound proxy）

让**所有拨出请求（REGISTER + INVITE）都先经过 `internal-lo`**，使拨出呼叫的 dialog 与注册落在**同一个 profile**：

```c
/* pj_phone_init() 里，账户配置段 */
pjsua_acc_config_default(&acc_cfg);
acc_cfg.id     = pj_str("sip:1000@192.168.23.6");  /* LAN 域（To 域，路由用） */
acc_cfg.reg_uri = pj_str("sip:10.0.2.2:5060");      /* loopback 腿（注册走这里） */
/* 新增：出站代理 —— 所有出站请求(REGISTER+INVITE)都先发给 internal-lo */
acc_cfg.proxy[acc_cfg.proxy_cnt++] = pj_str("sip:10.0.2.2:5060");
```

**为什么这样有效**（关键机理）：
- INVITE 的 **Request-URI 保持** `sip:1005@192.168.23.6`（还是路由到手机，不受影响）
- 但请求**第一跳发往** `10.0.2.2:5060`（internal-lo），FreeSWITCH 在 `internal-lo` 处理这条呼叫
- `internal-lo` 里能找到 1000 的 Contact（`127.0.0.1:15062`）→ 手机挂断时 BYE 发回**带端口的真实 Contact** → hostfwd → guest 正常收到
- 附带好处：出站请求源地址始终是 `127.0.0.1`（不在 `nat.auto` ACL）→ Contact 不会被 NAT 重写

**验证**（用户实测，2026-08-25）：
- 拨出方向：guest 拨 1005 → 手机接通 → 手机主动挂断 → guest 串口出现
  `call N disconnected: reason=200 (Normal call clearing) dur=xxxxms`，UI 回到 IDLE ✅
- 拨入方向：依然正常 ✅
- 注册仍 `200 OK`，冒烟无崩溃 ✅

### 4.5 经验总结（路由排查套路）

当"某方向挂断检测不到"时，按这个思路排查：

```
1. 看 FreeSWITCH 日志 "Sending BYE" 的目的地：
     带端口(真实 Contact) → 可达
     不带端口(纯 AOR)    → 路由失败，注册表里找不到该分机的 Contact
2. 确认「注册」和「呼叫 dialog」是否在同一个 profile
3. 若不在 → 用出站代理(outbound proxy)把拨出请求路由到注册所在的 profile
4. 同时检查 Contact 是否被 NAT 重写（源地址进没进 nat.auto ACL）
```

---

## 5. 运行期：来电打不通（"不在服务中"）

**现象**：手机拨 1000 提示"不在服务中"；即便绕过注册表直接 INVITE 能通，手机挂断的 BYE 也回不来。

**根因**：**域不匹配**。
- guest 注册在 `internal-lo`（10.0.2.2 域）
- 手机在 `internal`（192.168.23.6 域）
- FreeSWITCH 拨号计划在主叫域里查 `sofia_contact(1000@192.168.23.6)` → 查不到（1000 注册在别的域）→ "not in service"

**解决**（与 §4 是配套的两步）：

```c
acc_cfg.id     = pj_str("sip:1000@192.168.23.6");  /* ← To/注册域改成 LAN 域(与手机同域) */
acc_cfg.reg_uri = pj_str("sip:10.0.2.2:5060");     /* ← 但 REGISTER 仍走 loopback 腿 */
```

- **`id`（注册域）** = LAN 域 → FreeSWITCH 把 1000 绑定到 LAN 域 → 手机域的 `sofia_contact` 能找到它
- **`reg_uri`（注册目标）** 仍走 `10.0.2.2` loopback → 请求源是 `127.0.0.1`（不在 `nat.auto`）→ 注册表里的 Contact 保持 `127.0.0.1:15062`，不被 NAT 重写成不可达地址

> 若直接注册到 LAN 域（reg_uri = `192.168.23.6:5060`），`internal` profile 的 `apply-nat-acl=nat.auto` 会把 Contact 重写掉 → 反而不可达。**"loopback 腿 + LAN 注册域" 是这套 slirp/NAT 拓扑下唯一正确的组合。**

**验证**：手机拨 1000 正常响铃、接通；挂断可感知。

---

## 6. 附：其他已记录的坑

| 坑 | 说明 / 规避 |
|---|---|
| 新增 `.c` 不参与构建 | `file(GLOB)` 不自动刷新 → 重跑 `cmake -B build-phone -S .` configure |
| UI 文案乱码 | 项目字体仅 Montserrat 无 CJK → 全用 ASCII |
| `pj_thread_this()` 为 NULL | 新任务调 pjsua 前先 `pj_thread_register`（memset 的 desc，无 `pj_bzero`） |
| lwip 后台测试抢 RX | 电话模式跳过 HTTP 下载 + ping |
| 通话时长显示 | CONFIRMED 记 `g_call_start`，DISCONNECTED 用 `PJ_TIME_VAL_SUB` 算差 |
| mpsx 采集静音 | `snd_use_sw_clock=PJ_FALSE`（软件时钟与 DONE 中断不同步） |
| 回声收敛不了 | Speex AEC：`pjsua_set_ec(200,0)` 需在媒体 ACTIVE 后显式调用（pjsua 不自动建 EC） |
| null 设备静音丢 RTP | `no_vad=TRUE` + `snd_auto_close_time=-1` |
