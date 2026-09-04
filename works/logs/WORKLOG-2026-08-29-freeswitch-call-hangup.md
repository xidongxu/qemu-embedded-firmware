# WORKLOG 2026-08-28/29 FreeSWITCH 通话 32 秒自动挂断问题全解

## 0. 摘要

QEMU（mps2-an505 + slirp user 网络）里的嵌入式 pjsua 电话（guest）拨 FreeSWITCH
分机/echo 后，**通话建立、媒体双向正常，但约 32 秒后必然被 FreeSWITCH 主动挂断**。
经层层排查（UDP 命令通道、filter-dump 抓包、ESL 事件、RTP 互相关），根因**不是媒体问题**，
而是：

1. **guest 从未对 INVITE 的 200 OK 发送 ACK**（ACK 被发到了 guest 自身的 127.0.0.1 回环，
   根本没出网卡）→ FS 指数退避重传 200 OK → 约 32s 超时 BYE 挂断。
2. 此前还叠加一个 **RTP 出方向死锁**（FS SDP c=127.0.0.1 → guest 把 RTP 发到自己回环）。

根因本质是 **QEMU slirp user 网络单向性 + 为兼容 hostfwd 而做的 127.0.0.1 伪装**，
导致"127.0.0.1"在 FS 侧（宿主回环可达 guest）与 guest 侧（自己的回环）**语义分裂**。

解决时写了三个 pjsua/pjsip 补丁打通出方向，验证 60s+ 稳定通话；但**因属于 QEMU 环境特例、
不通用、且需改动 pjproject 开源库源码**，最终决定**还原全部补丁，改用 tap/桥接网络根治**。

---

## 1. 背景与现象

- 固件：`boards/mps2-an505/FreeRTOS`，pjsua 电话应用（`application/pj_phone.c`）。
- 宿主机：Windows，运行 FreeSWITCH 1.11.2（internal-lo profile，`sip-ip=127.0.0.1`，
  `rtp-ip=127.0.0.1`，`ext-rtp-ip=10.0.2.2`）。
- QEMU：`qemu-system-arm -machine mps2-an505 -cpu cortex-m33 -m 16M`，
  `-nic user`（slirp），带 `hostfwd=udp::15062-:15062 / 4000-:4000 / 15000-:15000`。
- 现象：拨 1007 或 echo 9196，能听到 FS 侧语音（FS→guest 通），媒体计数 rx/tx 同步增长、
  loss=0，但 **约 32 秒后被 FS 主动 BYE 挂断**（`Hangup cause: NORMAL_UNSPECIFIED`）。

## 2. 环境与拓扑

```
                     slirp (user 网络, 10.0.2.0/24)
guest (QEMU) 10.0.2.15 ─────► 10.0.2.2 (网关/宿主) ──► 宿主回环 127.0.0.1 ──► FreeSWITCH
   SIP   :15062  ─────────────────────────────────────────► 127.0.0.1:5060 (hostfwd)
   RTP   :4000   ◄── hostfwd udp::4000 ◄────────────────── 127.0.0.1:4000 (FS 发来)
   命令  :15000  ◄── hostfwd udp::15000 ◄────────────────── 宿主脚本
```

关键特性：
- slirp 是**单向**的：guest 能出（经 10.0.2.2），宿主**不能直接路由到** 10.0.2.15，
  只能靠 hostfwd 端口转发。
- 因此 guest 对 FS 表现为 `127.0.0.1`（SIP 源、SDP c=、Contact 全部是 127.0.0.1）。

## 3. 调试基础设施（本次新建）

为能自主拨号/抓取，建立了可靠的控制与诊断通道：

| 工具 | 作用 |
|---|---|
| `works/tools/phone_udp.py` | 通过 hostfwd UDP 15000 驱动 guest（dial/hangup/status/stat） |
| `boards/.../application/phone_net.c` | guest 端 UDP 命令服务器（15000），`main.c` 创建任务 |
| `works/tools/grab_serial.py` | 抓 guest 串口（tcp:2345）日志 |
| `works/tools/fs_esl.py` | FreeSWITCH ESL（8021/ClueCon）查询/订阅事件 |
| `works/tools/parse_pcap.py` / `sip_flow.py` / `rtp_analyze.py` / `rtp_corr.py` / `rtp_payload.py` / `rtp_timing.py` | filter-dump 抓包的各种分析 |
| `udptest` 命令（phone_net.c 内置） | guest 发 UDP 探针，确认 slirp 投递源/目标 IP |

> 说明：最初尝试串口 CLI（`phone_cmd.c`），但 QEMU TCG 下 TCP 串口流控链不可靠
> （guest 只收到 1 字节），故弃用改 UDP 通道，`phone_cmd.c` 已删除。

## 4. 排查过程

### 4.1 串口命令通道 → 不可靠
- 实现 `phone_cmd.c`（CMSDK UART 正确寄存器判断 + RX_EN），但 TCG 下串口流控链断，
  guest 收不到完整命令。
- **结论**：弃用串口，改用 UDP 命令通道。

### 4.2 UDP 命令通道建立
- 新增 `phone_net.c`（UDP 15000 服务器），宿主用 `phone_udp.py` 驱动。
- 关键：`phone_net_task` 需 `pj_thread_register()` 才能调 pjsua 的流统计 API。

### 4.3 发现 RTP 端口递增问题
- 观察 `stat`：每次通话 RTP 端口 4000→4002→4004 递增，而 hostfwd 只转发 4000，
  → 第二通起 FS→guest RTP 收不到（rx=0）。
- **修复**：`pjsua_media.c` `create_rtp_rtcp_sock` 固定 `next_rtp_port = cfg->port`（4000）。
- 修复后 FS→guest 通（rx 持续增长）。

### 4.4 "1007 不可达"的误导 → 改用 echo 9196
- 拨 1007 听到"不可达"提示音——其实是 **1007 未注册**，FS 走 voicemail 提示音流程，
  **不是媒体问题**（这反而证明 FS→guest 音频通）。
- **改用 echo 9196**（无需注册）测媒体。

### 4.5 FS 日志：SDP c=127.0.0.1 死锁
- FS console（loglevel 7）显示：
  - guest SDP `c=IN IP4 127.0.0.1`（guest public_addr=127.0.0.1）
  - FS `Local SDP c=IN IP4 127.0.0.1`（FS rtp-ip=127.0.0.1）
  - FS `AUDIO RTP ... -> 127.0.0.1 port 4000`（FS 发 RTP 到 guest hostfwd，通）
- **死锁**：guest 按 FS SDP c=127.0.0.1 发 RTP → 发到 guest 自己回环 → FS 收不到 → 32s 超时挂断。
- 子代理研究：`sip_force_nat_mode` / `rtp_adv_audio_ip` / `ext-rtp-ip` 都绕不过
  `switch_core_media_check_nat()` 对 127.0.0.1 的"一票否决"（FS 认为 127.0.0.1 无需 NAT，
  永远用 rtp-ip 生成 SDP c=）。**SDP 配置层面无解**。

### 4.6 强制 RTP 发送目标（方案 A：guest 发 10.0.2.2）
- 改 `pjsua_media.c` `apply_med_update()`：`pjmedia_stream_info_from_sdp()` 后，
  若 `rem_addr/rem_rtcp` IP==127.0.0.1 强制改写为 **10.0.2.2**（保留端口）。
- 效果：guest 发 RTP 到 10.0.2.2:FS_port。rx/tx 都增长，但 **32s 仍挂断**。
- 疑点：FS 是否真收到 guest RTP？（rx 增长可能只是 FS 发静音帧，不是 echo。）

### 4.7 public_addr=10.0.2.15（方案 B）失败
- 把 guest RTP `public_addr` 改为 10.0.2.15，期望 FS 直发 guest。
- FS 确实发到 `10.0.2.15:4000`，但 **guest rx=0**（宿主无 10.0.2.0/24 接口，包进黑洞）。
- 同时 FS 出现 `RTCP packet not written`（发往 10.0.2.15:4001 失败）。
- **结论**：方案 B 死路，FS→guest 必须 hostfwd，guest SDP c= 必须 127.0.0.1。

### 4.8 slirp 投递源 IP 的铁证（udptest）
- 给 `phone_net.c` 加 `udptest <port>`：guest 发 UDP 到 10.0.2.2:<port>。
- 宿主分别用 `0.0.0.0` 和 `127.0.0.1`-only 绑定监听，均收到：
  `GOT from 127.0.0.1:xxxxx`。
- **铁证**：slirp 把"guest→10.0.2.2"的 UDP 投递到宿主时，**源 IP = 127.0.0.1**。
  → 只要 guest SDP c=127.0.0.1，FS 的 RTP 源校验（期望 guest SDP c=）就匹配。

### 4.9 回到方案 A + 新发现：ACK 缺失
- 抓包（filter-dump）确认 guest 发 RTP 到 `10.0.2.2:FS_port`（768 包），方向正确。
- 但 FS 还是 32s 挂断。分析 SIP 流程（`sip_flow.py`）发现**异常**：
  - 周期性 `200 OK (CSeq 30274 INVITE)` 重传（间隔 1,2,4,4,4...s 指数退避）
  - **guest 从未发送确认 200 OK 的 ACK！**
- 200 OK 的 `Contact: sip:9196@127.0.0.1:5060`——guest 按 SIP 规则把 ACK 发到
  **127.0.0.1:5060 = guest 自身回环**，ACK 出不了网卡（filter-dump 抓不到）。

### 4.10 根因确认
- **FS 因收不到 ACK 而指数退避重传 200 OK，约 32s 后判定呼叫失败 → BYE 挂断**。
- 媒体双向其实完全正常（后续 RTP 互相关 0.997 证明 FS 100% echo 了 guest 的 1kHz 音频）。

## 5. 根因总结

| 层面 | 问题 | 本质 |
|---|---|---|
| **ACK 出方向** | FS 200 OK Contact=127.0.0.1:5060，guest ACK 发到自己回环 | slirp 单向 + 127.0.0.1 伪装 → "127.0.0.1" 两端语义分裂 |
| **RTP 出方向** | FS SDP c=127.0.0.1，guest RTP 发到自己回环 | 同上 |
| **RTP 入方向** | FS 发 127.0.0.1:4000 靠 hostfwd 转进 guest | 这方向天然通（设计如此） |

**一句话**：QEMU slirp 的 user 网络是"guest 出、hostfwd 进"的单向模式；为了让 FS 能
"回到"guest，把 guest 伪装成 127.0.0.1；于是 FS 给 guest 的所有地址都是 127.0.0.1，
而 guest 侧 127.0.0.1 是自己的回环 → 出方向的包（ACK、RTP）全死在回环。

## 6. 解决方案详解（四个补丁，均已验证后还原）

> **若后续不采用 tap/桥接、继续用 slirp+hostfwd 拓扑**，遇到同样的"通话 32s 挂断 /
> 单向媒体"问题，按下面 6.1~6.4 原样打上即可临时规避。
> 所有补丁均有 `=== QEMU/slirp workaround ===` 注释标记，且条件严格限定
> "对端地址 == 127.0.0.1"，**不影响真实网络下的标准行为**（真实网络里对端不会是 127.0.0.1）。

### 6.1 补丁 A：guest SDP 广告 127.0.0.1（应用层，打通 FS→guest RTP）

- **文件**：`boards/mps2-an505/FreeRTOS/application/pj_phone.c`
- **位置**：`pj_phone_init()` 账号配置处（`acc_cfg.rtp_cfg.public_addr` 一行，参考行 ~825）
- **修改**：`acc_cfg.rtp_cfg.public_addr = pj_str("127.0.0.1");`（本次保持基线，无需改动）
- **功能**：guest 在 SDP 的 `c=` 广告 `127.0.0.1:4000`，FS 据此把 RTP 发到宿主回环
  `127.0.0.1:4000`，经 hostfwd `udp::4000-:4000` 转进 guest → 打通 **FS→guest**。
- **坑**：**不要**改成 `10.0.2.15`——宿主没有 `10.0.2.0/24` 接口，FS 发往 10.0.2.15
  的包会进黑洞（实测 guest `rx=0`，FS 报 `RTCP packet not written`）。

### 6.2 补丁 B：固定 RTP 端口 4000（库内，防 hostfwd 失配）

- **文件**：`libutils/pjproject/pjsip/src/pjsua-lib/pjsua_media.c`
- **位置**：`create_rtp_rtcp_sock()` 函数开头、STUN 解析之后（参考行 ~457-461）
- **修改前**（标准 pjsua：`next_rtp_port` 每次调用后 +2 递增 → 4000, 4002, 4004 ...）：
  ```c
  if (acc->next_rtp_port == 0 || cfg->port == 0) {
      /* ... 用 cfg->port 初始化 ... */
  }
  ```
- **修改后**（在 if 之前插入一行强制固定为配置端口）：
  ```c
  /* QEMU hostfwd: always reuse the fixed configured RTP port (4000) so the
   * hostfwd UDP rule keeps matching across calls.  Without this, pjsua
   * increments next_rtp_port every call (4000 -> 4002 -> 4004 ...) and the
   * hostfwd'd 4000/4001 no longer reach the guest's RTP socket. */
  acc->next_rtp_port = (pj_uint16_t)cfg->port;

  if (acc->next_rtp_port == 0 || cfg->port == 0) {
      /* ... */
  }
  ```
- **功能**：让每次通话都复用 `4000/4001`，hostfwd 规则 `udp::4000-:4000` 持续匹配；
  否则第二通起 RTP 端口递增，FS→guest 的 RTP 到不了 guest（实测 `rx=0`）。

### 6.3 补丁 C：强制 guest 出方向 RTP/RTCP 指向 10.0.2.2（库内，打通 guest→FS RTP）

- **文件**：`libutils/pjproject/pjsip/src/pjsua-lib/pjsua_media.c`
- **位置**：`apply_med_update()` 的 audio 分支，`pjmedia_stream_info_from_sdp()` 成功返回、
  `stream_info.info.aud = asi; enc_name = &asi.fmt.encoding_name;` 之后（参考行 ~4094-4130）
- **修改前**：
  ```c
  stream_info.info.aud = asi;
  enc_name = &asi.fmt.encoding_name;
  ```
- **修改后**（插入一段，把对端 RTP/RTCP 的 127.0.0.1 改写为 10.0.2.2，端口保留）：
  ```c
  stream_info.info.aud = asi;
  enc_name = &asi.fmt.encoding_name;

  /* === QEMU/slirp workaround ===
   * FS 的 SDP c= 是 127.0.0.1（guest 自身回环），若照发 RTP 会发到 guest 自己。
   * 强制对端 RTP/RTCP 目标 IP 改为 slirp 网关 10.0.2.2（保留端口）。
   * slirp 投递到宿主时源 IP=127.0.0.1（已验证），匹配 guest SDP c=127.0.0.1，
   * 故 FS 的 RTP 源校验通过。 */
  {
      pj_str_t st_loop = pj_str("127.0.0.1");
      pj_str_t st_gw   = pj_str("10.0.2.2");
      pj_in_addr loop_addr, gw_addr;

      pj_inet_aton(&st_loop, &loop_addr);
      pj_inet_aton(&st_gw, &gw_addr);

      if (asi.rem_addr.addr.sa_family == PJ_AF_INET &&
          asi.rem_addr.ipv4.sin_addr.s_addr == loop_addr.s_addr)
      {
          asi.rem_addr.ipv4.sin_addr.s_addr = gw_addr.s_addr;
          PJ_LOG(4,(THIS_FILE,
                    "QEMU: forced RTP send target to %s:%d",
                    "10.0.2.2", pj_ntohs(asi.rem_addr.ipv4.sin_port)));
      }
      if (asi.rem_rtcp.addr.sa_family == PJ_AF_INET &&
          asi.rem_rtcp.ipv4.sin_addr.s_addr == loop_addr.s_addr)
      {
          asi.rem_rtcp.ipv4.sin_addr.s_addr = gw_addr.s_addr;
          PJ_LOG(4,(THIS_FILE,
                    "QEMU: forced RTCP send target to %s:%d",
                    "10.0.2.2", pj_ntohs(asi.rem_rtcp.ipv4.sin_port)));
      }
  }
  ```
- **功能**：guest 出方向 RTP/RTCP 发送目标从 `127.0.0.1`（FS SDP c=，=guest 回环）改为
  slirp 网关 `10.0.2.2` → slirp 转发宿主 `127.0.0.1:FS_port` → FS 收到（投递源 IP=127.0.0.1
  匹配 guest 广告的 SDP c=，通过 FS 源校验）→ 打通 **guest→FS RTP**。
- **关键点**：`pjmedia_stream_info` 里对端 RTP 地址字段是 **`rem_addr`**（不是 rem_rtp），
  RTCP 是 **`rem_rtcp`**；都是 `pj_sockaddr`，IPv4 用 `.ipv4.sin_addr.s_addr` 比较/赋值。
  **端口必须保留**（FS 每次通话动态分配 m= 端口）。

### 6.4 补丁 D：强制 in-dialog 请求（ACK/BYE/UPDATE）指向 10.0.2.2（库内，打通 guest→FS SIP）

- **文件**：`libutils/pjproject/pjsip/src/pjsip/sip_dialog.c`
- **位置**：`pjsip_dlg_send_request()` 函数开头：`pj_log_push_indent(); PJ_LOG(5,...)` 之后、
  `pjsip_dlg_inc_lock(dlg);` 之前（参考行 ~1375-1400）
- **修改前**：
  ```c
  pj_log_push_indent();
  PJ_LOG(5,(dlg->obj_name, "Sending %s",
            pjsip_tx_data_get_info(tdata)));

  /* Lock and increment session */
  pjsip_dlg_inc_lock(dlg);
  ```
- **修改后**（插入一段，把 Request-URI 的 host 127.0.0.1 改写为 10.0.2.2）：
  ```c
  pj_log_push_indent();
  PJ_LOG(5,(dlg->obj_name, "Sending %s",
            pjsip_tx_data_get_info(tdata)));

  /* === QEMU/slirp workaround ===
   * FS 200 OK 的 Contact 是 127.0.0.1:5060（guest 自身回环），ACK/BYE 照发会死在自己
   * 回环；FS 收不到 ACK → 指数退避重传 200 OK → ~32s 挂断。把 in-dialog 请求
   * Request-URI 的 host 改写为 slirp 网关 10.0.2.2。 */
  {
      pjsip_sip_uri *suri = (pjsip_sip_uri*)
                            pjsip_uri_get_uri(msg->line.req.uri);
      if (PJSIP_URI_SCHEME_IS_SIP(suri) &&
          suri->host.slen == 9 &&
          pj_memcmp(suri->host.ptr, "127.0.0.1", 9) == 0)
      {
          pj_strdup2(tdata->pool, &suri->host, "10.0.2.2");
          pjsip_tx_data_invalidate_msg(tdata);   /* 强制重打印消息 */
          PJ_LOG(3,(dlg->obj_name,
                    "QEMU: rewrote in-dialog request target 127.0.0.1 -> "
                    "10.0.2.2 (%s)",
                    pjsip_tx_data_get_info(tdata)));
      }
  }

  /* Lock and increment session */
  pjsip_dlg_inc_lock(dlg);
  ```
- **功能**：所有 in-dialog 请求（对 2xx 的 **ACK**、**BYE**、UPDATE、re-INVITE）的
  Request-URI 目标从 `127.0.0.1:5060` 重定向到 `10.0.2.2:5060` → 出 guest 网卡 → slirp →
  宿主 FS。**这是解决"32s 挂断"的关键补丁**（否则 ACK 永远到不了 FS）。
- **关键点**：请求行 URI 是 `tdata->msg->line.req.uri`；`pjsip_uri_get_uri()` 剥壳后若是
  sip: URI，其 `host` 字段可改；改完**必须** `pjsip_tx_data_invalidate_msg()` 强制重打印，
  否则实际发出的消息文本仍是旧的 127.0.0.1。

### 6.5 依赖关系与验证

- 四个补丁分工：A/B 管 **FS→guest**（入方向，hostfwd），C/D 管 **guest→FS**
  （出方向，把 127.0.0.1 扳到网关 10.0.2.2）。缺 C 则 FS 收不到 RTP（echo 无声），
  缺 D 则 FS 收不到 ACK（32s 挂断）。
- **验证**：A+B+C+D 打上后，echo 9196 通话 60s+ 不挂断（rx/tx 持续增长、loss=0），
  pcap 见 `ACK sip:9196@10.0.2.2:5060`，RTP 互相关 0.997。
- **当前状态**：四个补丁**已全部还原**，pjproject 源码为上游原样
  （`grep -E "QEMU|hostfwd" pjsua_media.c` 无残留）。

## 7. 验证

- echo 9196：`rx/tx` 从 139→2965 持续增长，`loss=0`，**60s+ 不挂断**（之前 32s 必挂）。
- pcap 确认：`ACK sip:9196@10.0.2.2:5060` 发出，**无 BYE**。
- RTP 质量：50.03 pps（20ms）、序列严格 +1、时间戳严格 +160、SSRC 恒定；
  FS→guest 与 guest→FS 的 1kHz payload **互相关 0.997**（FS 真 echo）。
- ESL `CHANNEL_HANGUP` 事件：修复前 `Hangup-Cause: NORMAL_UNSPECIFIED`（FS 主动）。

## 8. 后续决策：还原补丁，改用桥接根治

- 这三个补丁**不是 pjproject 的 bug 修复**，而是 QEMU+FS 127.0.0.1+slirp 环境的**特例**，
  不通用、需改开源库源码，维护成本高（升级 pjproject 要重放）。
- 因此决定：**全部还原**，改用 **tap/桥接网络**（guest 拿宿主同网段真实 IP，FS 直连，
  无 127.0.0.1 伪装，零补丁、pjsua 100% 标准行为）。
- 桥接后待办：
  1. 装 TAP 驱动 + 建网桥（**有线**网卡，无线桥接常失败）。
  2. QEMU `-nic user,...hostfwd...` → `-nic tap,ifname=tap0,script=no,downscript=no`。
  3. `pj_phone.c`：去掉 `rtp_cfg.public_addr=127.0.0.1`；`FS_HOST`/`reg_uri`/proxy 改用宿主真实 IP。
  4. 重新编译、注册、拨号验证。

## 9. 经验教训

1. **先确认"是否真通了"再看计数**：rx/tx 增长 ≠ 对端收到。FS 会持续发静音帧维持 RTP，
   单看计数会被误导。**内容互相关 / 对端日志**才是硬证据。
2. **SIP 的 ACK 是隐形杀手**：INVITE 200 OK 后 UAC 必须发 ACK，否则 UAS 会指数退避
   重传 200 OK 最终超时挂断；而"对端地址=自身回环"时 ACK 悄悄死在本地，极难察觉。
3. **slirp 是单向的**：guest 出、hostfwd 进；宿主永远无法直连 10.0.2.x。
4. **伪装地址是双刃剑**：127.0.0.1 让入方向通，但让出方向（按对端地址发送）全死。
5. **QEMU 环境特例 vs 标准行为**：网络拓扑问题应优先用"环境方案"（桥接）解决，
   避免往开源库里塞不通用的补丁。

## 10. 附录

### 10.1 关键诊断命令
```powershell
# 驱动 guest（UDP 15000）
python works/tools/phone_udp.py --cmd "status"
python works/tools/phone_udp.py --cmd "dial 9196" --poll 3 --polls 15
python works/tools/phone_udp.py --cmd "stat"

# FS 诊断（ESL 8021）
python works/tools/fs_esl.py "api status"
python works/tools/fs_esl.py "api show channels as json"

# 抓包（QEMU 启动参数加 -object filter-dump,...,file=dump.pcap）
python works/tools/sip_flow.py dump.pcap     # SIP 流程 / BYE 方向
python works/tools/parse_pcap.py dump.pcap   # UDP 流统计
python works/tools/rtp_corr.py dump.pcap     # echo 内容互相关
python works/tools/rtp_timing.py dump.pcap   # RTP 发送速率
```

### 10.2 关键证据（本次抓包，已按清理策略删除，可重新生成）
- ACK 缺失：SIP 流程里 200 OK 重传（同 CSeq）且无对应 ACK。
- slirp 源 IP：`udptest` → 宿主 127.0.0.1-only 绑定收到，源=127.0.0.1。
- echo 互相关 0.997：媒体双向完全正常。

### 10.3 保留的工具
`works/tools/` 下脚本为通用诊断/测试工具，桥接后继续使用：
`phone_udp.py`、`fs_esl.py`、`grab_serial.py`、`parse_pcap.py`、`sip_flow.py`、
`rtp_analyze.py`、`rtp_corr.py`、`rtp_payload.py`、`rtp_timing.py`、`send_rtp.ps1`、
`send_udp.ps1`、`make_sine_wav.py`、`run_phone_fs_test.ps1` 等。
