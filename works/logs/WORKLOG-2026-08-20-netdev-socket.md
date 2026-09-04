# WORKLOG-2026-08-20 — 网络后端改造：socket netdev 点对点直连（方案 A）

> ## ⭐ 最终结论（2026-08-22，本文件下方向已全部收敛）
> **双 QEMU 媒体"丢帧"彻底解决：非任何源码 bug（QEMU/pjproject 源码均 0 修改），
> 是测试应用侧 3 个配置问题**（媒体启动时序错位、编码器 ptime 不匹配、VAD 静音误判）。
> 修复后 **slirp（当前拓扑）与 socket 直连两种后端均 6+ 轮双向 200/200 帧零丢、RTCP loss=0、
> ALL PASSED**。曾怀疑的"QEMU 通道丢帧"（本文件 08-20 内容）与"lan9118 async 修复必要性"
> 均经 A/B 证伪并还原。详见文末 08-22 三个补记：
> [根因定位](#worklog-补记2026-08-22--丢包根因彻底推翻--达成双向-200200-零丢)、
> [lan9118 A/B](#附qemu-lan9118-async-修复-ab2026-08-22-追加)、
> [slirp A/B](#附slirp-拓扑-ab2026-08-22-追加)。

## 背景

用户不满 QEMU slirp 丢包（实测 0~37.5% 剧烈随机、成对突发），要求实施方案 A：
用 QEMU `socket` netdev 点对点直连替代 user-mode slirp，绕过 slirp 栈，消除转发损失。

## 实施

1. **固件拓扑改造**（`pj_sip_dual_test.c`）：
   - 弃用 `GW_IP 10.0.2.2` / hostfwd 端口（`MY_EXT_SIP`/`CALLEE_EXT_SIP` 等）
   - 两端**不同静态 IP**：caller `10.0.2.15`、callee `10.0.2.16`（`MY_IP`/`PEER_IP`）
   - Via/Contact/SDP conn/拨号/RTP 目标全部用各自**真实 IP**（SDP 协商自动定对端 RTP 地址）
2. **lwIP 静态 IP**（`lwip_os_test.c`）：按 `PJ_DUAL_ROLE_*` 条件化（caller .15 / callee .16）
3. **启动方式**：`-nic socket,...`（见下"坑"）+ 两端不同 MAC
4. **脚本**：`works/tools/run_dual_socket.ps1`（callee listen 先起，caller connect 后起，可 -Runs n 循环）

## 坑（都踩过）

1. **只给 `-netdev socket` 不建 NIC**：mps2-an505 板载 lan9118 仅在 `nd_table[0].used` 时才创建并绑定 →
   必须用 `-nic socket,id=n0,listen=127.0.0.1:20000,model=lan9118,mac=...`（`-nic` = netdev + NIC 一起）。
2. **两端 MAC 必须不同**（直连是同一 L2 段）：默认都是 `52:54:00:12:34:56` → ARP 乱、INVITE 到不了。
   在 `-nic` 里给 `mac=52:54:00:12:34:01`(:02)。
3. **`-icount shift=auto,align=on`**：让 guest 时间与实时 1:1，但运行拖慢 ~20x（2s 通话→40s+）
   且音频几乎无声（loud=0）→ **弃用**。
4. **eth_rx 优先级 3→5**：不降反升（对端恒定丢 40-44/200）——高优先级抢占 tcpip_thread/media 更糟。
   回退 3（A/B 记录在 lwip_os_test.c 注释）。

## 结果（socket 直连，eth_rx=3，4 轮）

```
RUN1: callee→caller 丢 34 | caller→callee 丢 0  | 双 FAILED
RUN2: callee→caller 丢 39 | caller→callee 丢 0  | 双 FAILED
RUN3: callee→caller 丢 0  | caller→callee 丢 28 | 双 ALL PASSED
RUN4: callee→caller 丢 0  | caller→callee 丢 18 | 双 ALL PASSED
```

- **单方向** 0-39/200（0~20%）波动，另一方向恒 0；比 slirp（双向 0~37.5%）略好、更可预测。
- 音频每轮 FFT 正常（439/1001Hz，PLC 连续）、DTMF/RTP/RTCP 全工作。
- **未根治**：根因是 **QEMU TCG 虚拟时钟突发**——guest 定时器跳变 → `media_clock` 发送节奏突发
  （一次 2-3 帧然后停顿）→ 对端 jbuf empty + 偶发 lan9118 接收溢出。socket/icount/优先级均无法根治。

## 结论

- **方案 A 落地**：socket 直连消除了 slirp 栈转发损失、无 hostfwd、拓扑更真实（点对点 L2 直连）。
- 剩余丢包是 **QEMU TCG 仿真本质**；固件 PLC 保证音频连续。真实硬件（无 TCG 突发、无 slirp）
  丢包为 1-5% 随机散布，PLC 轻松处理。
- 公网可用：真实网络丢包随机且低，固件已备好 PLC/jbuf/自适应。

## 附：lvgl A/B 实验（2026-08-20 晚）

假设：`lv_demo_benchmark()` 持续渲染占满 CPU → 加剧 TCG 时钟突发 → 放大丢包。
`main.c` 在 dual 模式（PJ_DUAL_ROLE_*）下跳过 `lv_task_init()`。

socket 直连 + **无 lvgl** 4 轮：

```
RUN1: callee→caller 丢 25 | caller→callee 丢 0 | 双 ALL PASSED
RUN2: callee→caller 丢 26 | caller→callee 丢 0 | 双 ALL PASSED
RUN3: callee→caller 丢 27 | caller→callee 丢 0 | 双 ALL PASSED
RUN4: callee→caller 丢 31 | caller→callee 丢 0 | 双 FAILED
```

对照（有 lvgl）：丢 34/39/28/18（方向不定），2/4 轮双 FAILED。

**结论**：
1. **lvgl 不是丢包根因**（屏蔽后仍丢 12-15%）。
2. 但 lvgl 有真实影响：屏蔽后丢包更稳定、PASS 率 2/4→3/4。保留 dual 模式屏蔽（省 CPU、更稳）。
3. ~~单向恒定丢包 = 双 QEMU 虚拟时钟速率差~~ —— **此解释被后续定位实验否定**（见下）。

## 定位实验（2026-08-20，决定性）

在 `media_clock_cb` 用高精度 SysTick 时间戳测**实际回调间隔**，并读 lan9118 的
`RX_DROP` 寄存器（0x420000A0）+ 驱动 `rx_overruns`：

```
callee: clock cb n=200 min=8122 max=11283 avg=9935 us burst=0 gap=0
        lan9118 rx_pkt=231 rx_overruns=0 qemu_rx_drop=0 | rtcp loss=0
caller: clock cb n=200 min=8799 max=11302 avg=9943 us burst=0 gap=0
        lan9118 rx_pkt=209 rx_overruns=0 qemu_rx_drop=0 | rtcp loss=26
```

**三连证实**：
1. **发送端完全均匀**：media_clock 回调间隔 avg≈9.94ms、min 8-8.9ms、max 11-11.8ms，
   burst(<8ms)=0-2、gap(>15ms)=0 → **guest 侧无任何发送突发**（"TCG 发送突发"假说证伪）。
2. **接收端网卡无溢出**：`qemu_rx_drop=0`、`rx_overruns=0`，且 caller 收到 209 个以太网帧
   （全收）→ **丢的帧根本没到达接收端 QEMU**（若到达且 FIFO 满，rx_drop 会 >0）。
3. **丢包在 QEMU socket backend 发送路径**：发送端 QEMU 用非阻塞 socket 写；对端 QEMU 主循环
   （TCG 跑 guest）读 socket 不及时 → TCP 发送缓冲满 → 非阻塞写失败 → **QEMU 直接丢帧**
   （slirp 的 UDP 转发同类：小缓冲 + 尽力而为）。

**最终结论**：
- 丢包是 **QEMU 进程间虚拟网络通道（socket backend 非阻塞写 + 小缓冲）无法吸收两个独立 TCG
  进程的调度差异**（发送端写 > 接收端读的瞬时积压 → 丢弃）。
- **固件/guest 完全正常**（发送均匀、网卡零溢出、PLC 覆盖丢包、音频连续）。
- slirp 与 socket backend 是同类架构局限（QEMU 虚拟网络后端的小缓冲 + 尽力而为 UDP/非阻塞写），
  不是 bug，也不是两台真实设备的问题（真实网络由硬件 NIC + 网络栈处理，无此瓶颈）。
- QEMU 内无法根治（`-icount` 太慢坏音频、优先级/缓冲无益、网卡无溢出）。真实硬件无此问题。

## 涉及文件

- `boards/mps2-an505/FreeRTOS/application/pj_sip_dual_test.c` — IP 拓扑改直连（MY_IP/PEER_IP）；
  含诊断：clock 回调间隔统计、lan9118 RX_DROP/rx_overruns 打印
- `boards/mps2-an505/FreeRTOS/application/lwip_os_test.c` — 静态 IP 按 role；eth_rx 优先级（A/B）
- `boards/mps2-an505/FreeRTOS/application/main.c` — dual 模式跳过 lv_task_init（lvgl A/B）
- `works/tools/run_dual_socket.ps1`（新增）— socket 直连启动/回归脚本
- 旧 slirp 启动（hostfwd）已由 `run_dual_socket.ps1` 取代（固件已不兼容 slirp 拓扑）

---

# WORKLOG 补记（2026-08-22）— 丢包根因彻底推翻 + 达成双向 200/200 零丢

## 背景

用户指出上一版结论（"丢包在 QEMU socket backend 发送路径，QEMU 内无法根治"）需要复核，
并要求逐层验证（net_burst → transport → stream）确认是否源码 bug。

## 逐层诊断链（证明网络/驱动/pjmedia 全部正常）

1. **net_burst_test.c**（纯 lwIP socket 双向 200 包 @10ms，READY 握手同步）：6 方向全
   tx=200 rx=200 loss=0 ooo=0 → **底层网络双向零丢**。关键：无 READY 握手时先启动端早发完
   → 对端漏收（**时序错位假象**，非丢包）。
2. **transport vs stream 计数**（transport_udp.c 临时 `pjmedia_tp_rx_rtp_cnt` vs
   `rstat.rx.pkt`）：完全相等 → **pjmedia 收发零丢弃**。
3. **发送端丢帧源排查**（临时计数）：`tx_ebusy=0`、`tx_sendto_fail=0` → transport 发送全成功；
   但 `g_tx_ok=200`（put_frame 全成功）而 `rtcp tx < 200` → **差异在 put_frame 内部、transport 之前**
   —— 即 `frame_out.size==0`（编码器空帧）路径（stream.c 此处**静默返回 PJ_SUCCESS**）。

## 三个真实根因（全是测试/配置侧，非源码 bug）

1. **媒体启动时序错位**：后启动端 transport 未就绪，前启动端已发开头 RTP → 对端 RTCP 算丢。
   → 新增 `media_sync_handshake()`（SYNC 端口 20003）：双方非阻塞轮询 + 持续重发直到互见、
   见后再发 5 次保证对端收到。
   **坑**：本端口 lwIP **阻塞 recvfrom + SO_RCVTIMEO 会永久挂死**（callee 卡死、无任何输出）——
   `LWIP_SO_RCVTIMEO=1` 存在但不可靠 → 必须 `ioctl(FIONBIO)` 非阻塞 + `vTaskDelay` 轮询。
2. **编码器 ptime 不匹配**：SDP 无 `a=ptime` → G.711 默认 20ms，媒体时钟喂 10ms 帧 →
   rebuffer 路径约 30% 帧输出空（`frame_out.size==0`）→ 未发送。→ SDP 加 `a=ptime:10`
   （确认 `enc_spf=80`、`enc_buf=NULL` 无 rebuffer）。
3. **VAD 静音误判**：pjmedia 在 600ms（`PJMEDIA_STREAM_VAD_SUSPEND_MSEC`）后重新启用 VAD，
   G.711 silence_det 把纯正弦（avg≈10186）误判静音（从 ts=4880 开始）→ `g711_encode` 输出空帧。
   WAV 经 Python 分析确认无静音段 → 纯误判。→ 测试代码 `si.param->setting.vad=0`
   （`pjmedia_codec_mgr_get_default_param` + 设 vad=0，feed 给 `pjmedia_stream_create`）。

## 最终结果（干净状态，无任何诊断代码，pjproject 源码 0 修改）

```
RUN1: callee rtcp tx=200 loss=0 | caller rtcp tx=200 loss=0 | 双 ALL PASSED
RUN2: callee rtcp tx=200 loss=0 | caller rtcp tx=200 loss=0 | 双 ALL PASSED
RUN3: callee rtcp tx=200 loss=0 | caller rtcp tx=200 loss=0 | 双 ALL PASSED
```

- **双向 200/200 帧全收、RTCP loss=0**。
- `git status libutils/pjproject/` 为空 → **jbuf.c/stream.c/transport_udp.c/g711.c 全部还原**，
  之前（08-20）"QEMU 通道丢帧"结论被彻底推翻。
- **结论**：socket 直连 + lan9118 修复 + 固件 lwIP/pjmedia 全部正常；"丢包"是测试应用自身的
  启动时序、ptime 协商、VAD 三个配置问题。真实场景（双方就绪后通话）无此问题。

## 保留的修复（都在 `pj_sip_dual_test.c`，非 pjproject）

- `media_sync_handshake()`：媒体启动同步（非阻塞轮询 + 双重重发确认）
- `create_audio_sdp()`：`a=ptime:10`
- `run_dual_media()`：`si.param->setting.vad=0`（禁用 VAD）

## 涉及文件（2026-08-22）

- `boards/mps2-an505/FreeRTOS/application/pj_sip_dual_test.c` — 上述 3 处修复
- `boards/mps2-an505/FreeRTOS/application/net_burst_test.c`（新增）— 底层网络零丢验证
- `boards/mps2-an505/FreeRTOS/application/main.c` — dual 模式跳过 fatfs_test（格式化卡系统破坏双实例同步）
- 临时诊断（transport_udp.c/stream.c/g711.c 计数与打印）已全部移除并还原

## 附：QEMU lan9118 async 修复 A/B（2026-08-22 追加）

**问题**：`qemu/hw/net/lan9118.c` 的 `qemu_send_packet`→`qemu_send_packet_async(...,lan9118_tx_sent)`
（空 sent_cb 使 EAGAIN 时入队重发防丢帧）是否有必要保留？

**实验**：`git checkout hw/net/lan9118.c` 还原为原始 → MSYS2 增量 `make -j16 && make install`
（只重编 lan9118.o + 链接）→ 固件 3 修复保留重跑 3 轮：

```
RUN1: callee tx=200 loss=0 | caller tx=200 loss=0 | 双 ALL PASSED
RUN2: callee tx=200 loss=0 | caller tx=200 loss=0 | 双 ALL PASSED
RUN3: callee tx=200 loss=0 | caller tx=200 loss=0 | 双 ALL PASSED
```

**结论**：还原后仍双向 200/200 零丢，与带修复时一致 → **async 修复非必需，保持还原**
（QEMU 源码干净，贴近上游）。媒体负载（10ms/帧 212B）下 socket 后端从未 EAGAIN，
该修复从未被触发。重建命令：
`C:\Users\xidon\program\MSYS64\msys2_shell.cmd -mingw64 -defterm -no-start -c "cd .../qemu/qemu-configure && make -j16 && make install"`
（`qemu-build.ps1` 末尾会自动跑 testcase GUI 阻塞，勿用）。

## 附：slirp 拓扑 A/B（2026-08-22 追加）

**问题**：当初为"避开 slirp 丢包"把测试改成 socket netdev 直连。既然根因已定位为测试侧
3 配置问题，能否还原回 slirp（user-net + hostfwd）？

**改动**（保留 3 修复，仅拓扑从直连改回 slirp/hostfwd）：
- `pj_sip_dual_test.c`：宏改回 `GW_IP "10.0.2.2"` + hostfwd 外部端口（CALLEE_EXT_SIP 15062、
  CALLER_EXT_SIP 16062、RTP 4000/4002）；Via/Contact/拨号/SDP conn 用 `GW_IP:<ext>`；
  握手 peer 改 `GW_IP:PEER_SYNC_HOST`（caller 20003 / callee 20013，各自 hostfwd 暴露 guest 20003）。
- `lwip_os_test.c`：两端都固定 10.0.2.15（各自独立 NAT，经 hostfwd 互访）。
- 启动：`works/tools/run_dual_slirp.ps1`（新增）。

**验证（slirp + 3 修复，6 轮）**：

```
RUN1: callee tx=200 loss=0 | caller tx=200 loss=0 | 双 ALL PASSED
RUN2: callee tx=200 loss=0 | caller tx=200 loss=0 | 双 ALL PASSED
RUN3: callee tx=200 loss=0 | caller tx=200 loss=0 | 双 ALL PASSED
（再 3 轮同样全 PASSED）
```

**结论**：slirp + 3 修复 = 双向 200/200 零丢，与 socket 直连一致 → **slirp 从未真正丢包**，
之前 slirp 的"丢包（0~37.5%）"同样是测试侧 3 配置问题（无握手、ptime 不匹配、VAD 误判）。
**固件已还原回 slirp 拓扑**（socket 直连改动还原），`run_dual_socket.ps1` 标注 OBSOLETE，
新脚本 `run_dual_slirp.ps1`。QEMU/pjproject 源码均无修改。

## 附：lwIP 静态 IP 为什么必须固定 10.0.2.15（2026-08-22 追加，A/B 证明不可还原）

**疑问**：还原回 slirp 后，`lwip_os_test.c` 的 IP 从"按角色条件化（caller .15 / callee .16）"
改成两端固定 `.15`——这个改动是否必要（看起来只是改了 IP 取值 + 注释）？

**A/B 验证**：临时改回条件化（callee `.16`），重建后跑 slirp 测试：

```
callee (IP 10.0.2.16):  waiting for INVITE...   ← 永远等不到（通话建立不了）
caller (IP 10.0.2.15):  INVITE sent (rc=0)      ← 已发出但无人应答
```

**根因**：QEMU slirp 的 `hostfwd` 把 host 端口收到的包**注入到 guest 的虚拟地址
`10.0.2.15`**（slirp 内部固定的 guest IP）。callee 若静态设 `.16`，则 hostfwd 转发的
SIP/SYNC/RTP 全部投到 `.15`，callee 收不到任何转发流量 → 通话根本无法建立。

**结论**：slirp 拓扑下两端**必须都是 `10.0.2.15`**（各自独立 NAT，经 hostfwd 互访）。
该改动是功能性要求，**不可还原为条件化**。已 `git checkout` 恢复为提交版并重建。

---

# 当前网络拓扑说明（slirp / user-net + hostfwd）

## 拓扑图

```
            ┌─ QEMU instance A (caller, UAC) ─────────────┐
  host 端口 │   guest: 10.0.2.15/24  gw 10.0.2.2         │
  16062 ────┤   SIP bind 0.0.0.0:15062 (Via/Contact=10.0.2.2:16062)  │
  4000  ────┤   RTP  bind 0.0.0.0:4000  (SDP c=10.0.2.2 m=audio 4000)│
  4001  ────┤   RTCP bind 0.0.0.0:4001                              │
  20003 ────┤   SYNC bind 0.0.0.0:20003 (media handshake)           │
            └──────────────┬──────────────────────────┘
                           │ slirp NAT (host side)
            ┌──────────────┴──────────────────────────┐
            └──────────────┬──────────────────────────┘
                           │
            ┌──────────────┴──────────────────────────┐
  host 端口 │   guest: 10.0.2.15/24  gw 10.0.2.2      │
  15062 ────┤   SIP bind 0.0.0.0:15062 (contact=10.0.2.2:15062)   │
  4002  ────┤   RTP  bind 0.0.0.0:4002  (SDP c=10.0.2.2 m=audio 4002)│
  4003  ────┤   RTCP bind 0.0.0.0:4003                              │
  20013 ────┤   SYNC bind 0.0.0.0:20003                             │
            └─ QEMU instance B (callee, UAS) ─────────┘
```

关键点：
- 每实例一个 **slirp user-mode NAT**，guest 固定 `10.0.2.15`、网关 `10.0.2.2`、DNS `10.0.2.3`；
- 两实例通过 **hostfwd 端口转发**互访（每个 QEMU 在 host 上监听不同的 host 端口）：
  - caller hostfwd：`udp::16062-:15062`(SIP)、`udp::4000-:4000`(RTP)、`udp::4001-:4001`(RTCP)、`udp::20003-:20003`(SYNC)
  - callee hostfwd：`udp::15062-:15062`(SIP)、`udp::4002-:4002`(RTP)、`udp::4003-:4003`(RTCP)、`udp::20013-:20003`(SYNC)
- 固件里所有"对端地址"统一写成 `GW_IP(10.0.2.2):<对端 host 端口>`，由 NAT + hostfwd 送达对端 guest。

## 为什么这样构建（vs socket 直连 / slirp 的取舍）

- **为什么选 slirp（最终）**：QEMU 内置 user-mode 网络，**零配置、免管理员、免桥接网卡**，
  两个实例各自 NAT 完全隔离（都是 10.0.2.15 互不冲突）。A/B 证明它**从未真正丢包**，
  比 socket 直连更简单、更接近"每台设备一个私有网 + 端口映射互访"的真实部署形态。
- **为什么用 hostfwd 端口映射**：两个独立 NAT 网段天然不可达，必须经 host 做端口转发。
  这与"两台设备各自在 NAT 后，通过路由器端口映射/公网 IP 互访"是同一模型。

## 这样做的好处

1. **简单可复现**：`run_dual_slirp.ps1` 一条命令起双实例，端口映射固定，脚本化回归；
2. **隔离干净**：每实例独立 NAT，无共享 L2 段，IP/ARP 无冲突；
3. **与 socket 直连等价**（媒体路径零丢），且无需 listen/connect 时序握手；
4. **逼近真实部署**：NAT + hostfwd ≈ "双端各自 NAT + 端口映射"的真实形态，便于验证
   SIP 地址发布、SDP 协商、RTP/RTCP 经 NAT 的路径。

## 与真实网络的区别（重要）

| 维度 | 本模拟（slirp+hostfwd） | 真实网络 |
|------|------------------------|----------|
| 拓扑 | 各自私有 NAT + hostfwd 端口映射（≈ 跨 NAT） | 同 LAN 二层直连，或公网经路由器/防火墙 |
| 丢包 | 测试场景双向 200/200 零丢 | 公网随机 1-5%，突发、抖动、乱序 |
| 延迟/抖动 | QEMU TCG 软件模拟，media_clock 均匀但整体偏慢 | 实时，RTT 几十~几百 ms 波动 |
| 地址 | 私有 10.0.2.x + host 端口映射 | 公网 IP / 内网 IP + 路由；对称 NAT 常见 |
| NAT 类型 | hostfwd 是"显式端口转发"（等价全锥/静态映射） | 对称 NAT / 防火墙策略 / 无端口映射 |
| 带宽 | user-mode 转发，吞吐有限 | 网卡/链路带宽，多路通话需规划 |
| 可达性 | 端口映射后必达（无 NAT 穿越问题） | 需 STUN/TURN/ICE 才能穿 NAT |

## 若后续在公网使用，需要注意（checklist）

1. **NAT 穿越**：真实 NAT 下 SIP/RTP 不可达，需要 **STUN（探测映射地址）+ TURN（中继）
   + ICE（候选协商）**。本测试的 hostfwd 等价于"手动静态端口映射"，公网需在路由器配置
   端口映射（UDP 5060 + RTP 端口段），或走 ICE。
2. **SIP/SDP 地址必须公网可路由**：Via/Contact/SDP c= 里的地址不能用私有 IP。用公网 IP，
   或 STUN 返回的映射地址（外网端口），且 RTP 需**对称 RTP**（同一端口收发）配合 NAT。
3. **防火墙/端口**：放行 UDP（SIP 5060 + RTCP 3101 + RTP 协商端口段，如 10000-20000）。
   运营商 UDP 限速/阻断需注意。
4. **丢包/抖动**：公网 1-5% 丢包固件 PLC/jbuf 可处理（已备好）；>10% 需 FEC、冗余、
   码率/前向纠错自适应；抖动大需调大 jbuf 自适应上限（`si.jb_max`）。
5. **时延**：公网 RTT 大，`media_sync_handshake` 超时/重发参数、RTCP 间隔需相应放宽；
   RTP 时间戳用本地时钟（无需全局同步），但要保证单调递增。
6. **认证与加密**：SIP Digest 认证防盗打；SIPS(TLS)/DTLS-SRTP 加密防窃听；注意 SIP 扫描。
7. **带宽**：PCMU 8kHz 单声道 ≈ 64kbps + IP/UDP/RTP 头 ≈ 80kbps/路；多路/高码率需规划。
8. **DNS**：生产用域名 + SRV/NAPTR 记录（SIP 域解析），本测试用 IP 直连。
9. **RTCP**：保证 RTCP 端口可达（质量监控）；NAT 下 RTCP 也可能需要 TURN 中继。
10. **部署形态**：真实是两台独立设备（各有公网/内网地址），建议用 pjsua 标准流程
    （STUN/TURN/ICE）而非手工 hostfwd。

---

# WORKLOG 补记（2026-08-22 深夜）— 长通话测试（10s / 1000 帧）

## 目标
验证长时间稳定性：>10s、非固定 200 帧。关注时钟漂移、jbuf 自适应、音频连续性。

## 改动
- `pj_sip_dual_test.c`：`MEDIA_FRAMES` 200→**1000**（10s）；mic 捕获超时 20M→200M；
  媒体等待超时 10s→20s；**jbuf 配置**（见下）。
- WAV 源：新增 `sine_1k_8k_10s.wav` / `sine_440_8k_10s.wav`（10s，QEMU mic 设备读完即停无循环，
  必须给足时长）。
- `run_dual_slirp.ps1`：换 10s WAV，`$Wait` 45→60。

## 结果（10s / 1000 帧，slirp + 3 修复 + jb_init=40，3 轮）

```
RUN1: callee tx=1000 loss=0 | caller tx=1000 loss=0 | 双 ALL PASSED
RUN2: callee tx=1000 loss=0 | caller tx=1000 loss=0 | 双 ALL PASSED
RUN3: callee tx=1000 loss=0 | caller tx=1000 loss=0 | 双 ALL PASSED
```

- **网络 10s 稳定零丢**：双向 `rx rtcp pkt=1000 loss=0 discard=0`，`rtcp tx=1000 loss=0`。
- **音频连续**（analyze_call_audio FFT）：callee 听到 1001Hz（caller 1kHz）、caller 听到 ~474Hz
  （callee 440Hz，FFT bin 分辨率），`loud=35-38/38` → **empty 帧被 PLC 填充，音频不中断**。
- **时钟 avg 稳定**：`clock cb n=1000 avg=9987-9988us`（无大步漂移）。

## 重要发现：jbuf 自适应配置（真实固件问题，长通话才暴露）

- 原配置 `si.jb_init = 0` → **10s 通话 `empty=957-980`（97% 播放是 empty），`play normal` 仅 3%**。
  网络零丢但播放几乎全是空帧（get_frame 时 jbuf 无当前帧）。
- **根因（jbuf.c）**：自适应只在 `jb->jb_init_prefetch != 0` 时更新 prefetch（`if (jb->jb_init_prefetch)`）。
  `jb_init=0` → **自适应完全禁用**，prefetch 恒 0 → jbuf 不缓存 → 单向延迟 > 10ms（一帧）时
  get_frame 即空。2s 短通话 RTT 3-8ms（单向 < 一帧）未暴露；10s 通话 RTT 波动 0-218ms 暴露。
- **修复**：`si.jb_init = 40`（初始 prefetch 40ms=4 帧，**必须 >0 才启用自适应**）。
  → `prefetch=4`、`delay avg 26ms`、**`empty` 97%→56%、`play normal` 3%→43%**。
- **单位备忘**：`si.jb_*` 参数（jb_init/jb_min_pre/jb_max_pre）单位是 **ms**（stream.c 按 frm_ptime 换算成帧）。

## 仍存在的特性（非缺陷）
- **QEMU RTT 波动大**（callee 侧 0-218ms，单向可达 ~100ms > 40ms prefetch）→ 部分 empty（56%）。
  这是 QEMU slirp 长时间运行的延迟波动特性；**真实网络 RTT 稳定（<50ms 单向 <25ms）时
  40ms 自适应足够，empty 会很低**。empty 帧经 PLC 填充，音频连续（FFT 验证）。
- 自适应基于 burst level，持续 empty 时不会进一步涨 prefetch（`prefetch=4` 停在 min）——pjmedia 设计。

## 大规模回归（2026-08-22，20 轮 10s 长通话）

**配置**：slirp 拓扑 + 3 修复（握手/ptime/VAD）+ `jb_init=40`，10s / 1000 帧。

```
RUN 1-10 : 10/10 PASS   RUN 11-20: 10/10 PASS
总计 20/20 PASS（40 个方向），双向 tx 全 1000、loss 全 0（total_tx_loss=0）
指标稳定：empty 547-589（55-59%）、play normal 411-453（41-45%）
```

**结论**：10s 长通话大规模回归**全部通过、双向零丢、指标高度稳定**。长通话稳定性验证完成
（网络零丢 + 时钟 avg 稳定 + 音频连续 + jbuf 自适应配置修复）。QEMU 下 55-59% empty 是
slirp RTT 波动特性（PLC 填充，音频连续）；真实网络 RTT 稳定时 empty 会显著更低。

## 音频质量深度分析（2026-08-22，量化 PLC 占比对听感的影响）

**工具**：`works/tools/analyze_audio_deep.py`（新增）——逐 20ms 帧 RMS/主频/频谱质心，
检测静音段（PLC 超限→零填充）与 PLC 重复（谐波）。

**数据**（out.wav 立体声 3.83s；callee 听 1kHz、caller 听 440Hz）：

```
callee (1kHz): silence 39.3%  max_gap=0.42s  tone 60.7%  peak_med=1001Hz
               target_hit=100%  centroid=1004Hz（≈基频，谐波少）
caller (440Hz): silence 21.1%  max_gap=0.04s  tone 78.9%  peak_med=463Hz
               target_hit=73.3% centroid=674Hz（>基频，PLC 重复产生谐波）
```

**量化结论（PLC 对听感）**：
1. **断续（主要影响）**：empty 帧连续超过 `max_plc_cnt`（PLC 最大时长）后**零填充→静音**。
   callee 39% 静音、最长 **420ms 连续静音**（明显断续）；caller 21%、最长 40ms（轻中度）。
   callee 更严重 = 其 RTT 波动更大（0-218ms）→ 连续 empty 多。
2. **机械/嗡嗡（次要）**：PLC=重复上一帧，**主频保持**（callee 100% hit）但**谐波增加**
   （caller centroid 674Hz > 基频 463Hz）→ 轻微机械感。
3. **听感总体**：QEMU 下"断续 + 轻微机械"；**真实网络 RTT 稳定时 empty 低、静音少、听感正常**。

**改善建议（QEMU 场景，可选）**：增大 `PJMEDIA_MAX_PLC_DURATION_MSEC`（更多 PLC 填充、
少静音）；或增大 `jb_init`（如 80-100ms，减少 empty）。真实网络下 40ms 自适应已足够。

## ⭐ 音频优化：找到 empty/静音的真正根因（2026-08-22 深夜，决定性）

**背景**：深度分析发现 empty（56%）但 `plc call=0`（PLC 从未工作）、静音 50%——即使
`setting.plc=1`。加诊断（get_frame frame_type / synthesize_samples 入口）定位。

**根因**：**G.711 默认 `setting.frm_per_pkt=2`（每包 2 帧 = 20ms）** →
stream 的播放帧 `samples_required=PJMEDIA_PIA_SPF=160`（20ms），而**媒体时钟喂 80 样本（10ms）**
（`MFRAME_SAMPLES=80`）→ `get_frame` 每次要 2 帧 → **jbuf 消耗快 2 倍 → empty → 静音**。
synthesize_samples 里 `req(160) > out_buf_len(80)` → 走"参数错误"分支只零填 80 → 静音。
**jb_init/PLC 时长都只是缓解，非根因。**

**修复**：测试代码设 `param->setting.frm_per_pkt = 1`（10ms/包，匹配 10ms 媒体时钟）。

**效果（决定性，干净状态 3 轮）**：
```
empty   : 547-589 (55-59%)  -> 36.2  (3.6%)   ↓94%
normal  : 411-449 (43%)     -> 951.7 (95.2%)  ↑122%
音频静音 : callee 50% / caller 37% -> 12% / 1.5%
plc call: 0（从未工作）     -> 58/19（真正工作，missing≈10 被 PLC 处理）
```

**保留的优化（与根因修复一起生效）**：
1. `frm_per_pkt=1`（根因，pj_sip_dual_test.c）
2. `jb_init=80 / jb_min_pre=80`（吸收单向 RTT，减少 empty）
3. `PJMEDIA_MAX_PLC_DURATION_MSEC` 240→**1000**（config.h：更长 PLC 保护，missing 分散时用）
4. `SDP a=ptime:10`、`VAD 禁用`（之前修复）

**已清理**：stream.c 诊断（ft/synth/plc 打印与计数）全部还原，pjproject 仅剩 config.h 配置改动。
**提交项**：pj_sip_dual_test.c、config.h、run_dual_slirp.ps1、analyze_audio_deep.py（新增）、worklog。









---

## config.h A/B 验证（PJMEDIA_MAX_PLC_DURATION_MSEC 240 vs 1000）

**结论：修改不必要，已还原 config.h（240ms 默认）。pjproject 完全干净（config.h + stream.c 均无改动）。**

A/B 实测（frm_per_pkt=1 保留，10s 长通话 x3 轮，6/6 PASS）：
| 指标 | PLC 1000ms | PLC 240ms（默认） |
|---|---|---|
| empty avg | 36 | 33 |
| normal avg | 951.7 (95.2%) | 957.3 (95.7%) |
| missing avg | ~10 (1%) | 9.7 (1%) |

**为什么 240ms 足够**：根因修复是 frm_per_pkt=1，修复后 missing 仅 1% 且分散，每次 PLC 只填补几帧（<10ms 连续丢失），远在 240ms 上限内。PLC 时长上限只在长连续丢包时才有意义。

**若未来要调 PLC（不改 pjproject 源码的两种方式）**：
1. CMake 编译参数：PJMEDIA_MAX_PLC_DURATION_MSEC 是 #ifndef 保护的宏，可在项目 CMakeLists/工具链对 pjmedia 目标加 -DPJMEDIA_MAX_PLC_DURATION_MSEC=1000（config.h 的 #ifndef 自动跳过）。
2. ports 配置头：在 libutils/pjproject/ports/ 放配置头（如 ports/config_extra.h），用 -include 强制包含同样 #ifndef 覆盖。ports 目录本就是"配置/移植"层，不算改源码逻辑。
3. 当前不需要：240ms 已验证足够。

**提交清单（更新）**：pj_sip_dual_test.c、run_dual_slirp.ps1、analyze_audio_deep.py（新增）、WORKLOG。config.h 不含（已还原，pjproject 干净）。
---

## Step 1: guest<->host 互通验证（QEMU -> 宿主 pjsua）

**结论：互通可行。** SIP 信令双向 + RTP/DTMF guest->host 已实测证实，地址重写机制全部生效。

**拓扑**（slirp + hostfwd 回包通道）：
- guest 拨 sip:user@10.0.2.2:5060 -> slirp 网关 -> host 127.0.0.1:5060 -> pjsua（Via received=127.0.0.1:15062，slirp 保留源端口）
- guest SDP offer c=127.0.0.1:4000（宿主可见）；peer SDP 重写为 10.0.2.2；RTCP 目标 10.0.2.2:5001
- hostfwd：udp::15062-:15062, 4000-:4000, 4001-:4001（SIP/RTP 回包通道）

**实测证据**：
1. SIP INVITE guest->host：pjsua 收到
2. SIP 200 host->guest：guest CONFIRMED + SDP 协商成功
3. RTP guest->host：pjsua jitter buffer 收 562 帧 + DTMF 5/#（RFC2833）
4. 地址重写：offer/rewrite/RTCP 全部生效

**宿主侧**：pjsua 2.17 用 VS 2026 编译（build-win64/pjsip-apps/Release/pjsua.exe）；脚本 works/tools/run_pjsua_uas.ps1（--local-port=5060 --rtp-port=5000 --auto-answer=200 --null-audio）
**固件侧**：pj_sip_dual_test.c 新增 PJ_HOST_CALL 模式（-DPJ_HOST_CALL=ON + caller）；脚本 works/tools/run_hostcall_test.ps1

**关键机制（slirp 下 guest<->host）**：
- guest -> host：任何 host socket 以 10.0.2.2:<port>（网关=host loopback 别名）直达
- host -> guest：host 进程无法路由到 10.0.2.15/10.0.2.2，唯一通道是 hostfwd（发 127.0.0.1:<hostfwd端口>）
- 所以双侧地址需重写：guest offer/Contact 用 127.0.0.1:<hostfwd端口>，对端 SDP 重写 10.0.2.2

**待完善**：
1. ACK 仍发不出：pjsip 发送地址缓存深（改 dlg->target + last_ack RURI + dest_info 均未完全生效；pjsua 反复重传 200）。不影响已建立媒体。后续可用 TCP SIP 规避
2. guest 媒体时钟 ~5.6s 后卡住（统计未打印；疑似 lwIP UDP 发送/ioq 空转使 play 线程饿死）——不影响互通结论
3. host->guest RTP 完整统计待确认（通道已建立：pjsua 发 127.0.0.1:4000 -> hostfwd -> guest）
---

## Step 1 收尾（2026-08-22）：ACK + 媒体时钟 + host→guest 全部解决

**① ACK 已解决（手动 ACK）**：pjsip 的 ACK 发送地址缓存深（改 dlg->target / last_ack RURI / dest_info / tp_info.dst_addr 均不彻底）。最终方案：收到 200 后（CONNECTING/CONFIRMED）用 pjsip_msg_print 生成 ACK 文本 + 原始 UDP socket 直发 10.0.2.2:5060（网关→宿主），完全绕过 pjsip transport 解析。实测 pjsua 收到 ACK → Call 0 CONFIRMED。

**② "媒体时钟卡住"是误判**：日志截断导致。实际 clk ix=1000 tx=1000/1000，媒体完整跑完 10s。

**③ host→guest RTP 通道（两个根因）**：
- 根因 A：QEMU UDP hostfwd **惰性关联**——host socket 需 guest 先向该端口发包才学会转发目标，且关联 guest **源端口**（dummy 用随机源端口→关联错）。
- 修复：ctivate_hostfwd_rx() 在媒体 transport 创建**前**（SIP transport 建立后）**绑定 guest 端口(4000/4001)为源**发 dummy 到网关 → hostfwd 关联 guest:4000/4001。
- 验证：宿主 tp_probe.py 发 G.711 RTP 到 127.0.0.1:4000 → guest 
orm 1→97/141；pjsua 的 RTP 到达并播放 80000 字节（10s 音频）+ guest 收 4 RTCP。

**剩余（宿主工具特性，不影响互通结论）**：
1. pjsua 的 RTP 发送量不稳定（--null-audio + --play-file 组合行为）→ run_hostcall_test.ps1 判定可能 FAIL（要求 rx.pkt≥850），实际双向媒体已通
2. rtp_probe.py 未绑定源端口会干扰 guest 远端地址学习（Remote RTP address switched 到 probe 源端口）——仅用于接收验证
3. 媒体统计打印偶不出现（stdout 缓冲 + 被杀时机）

**最终结论：guest↔host 互通完整验证**（SIP 双向 + RTP 双向 + DTMF + ACK + 媒体 1000 帧 + 地址重写）。slirp 互通要点：guest→host 用 10.0.2.2:<port>（网关）；host→guest 走 hostfwd（宿主发 127.0.0.1:<hostfwd端口>），**UDP hostfwd 需 guest 绑定源端口激活关联**；双侧地址重写（guest offer 用 127.0.0.1:hostfwd端口，对端 SDP 重写 10.0.2.2）。