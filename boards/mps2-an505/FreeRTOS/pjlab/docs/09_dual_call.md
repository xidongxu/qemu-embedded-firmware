# 实验 09 — 真实 SIP 通话：双 QEMU / 与宿主互通（pj_sip_dual_test.c）

**阶段**：集成（stage 11）——信令 + 媒体一起，打真正的电话。

## 目的
把 01~08 全部串起来：**两个 QEMU 实例之间**（或 QEMU 与**宿主上的 pjsua**之间）真实拨号通话，走完整 SIP（INVITE/200/ACK）+ SDP 协商 + RTP 双向媒体 + DTMF。这是 pjlab 的"毕业实验"。

## 两种拓扑

### A. 双 QEMU（两个实例互拨）
每个 QEMU 独立 slirp NAT（guest 固定 `10.0.2.15`，网关 `10.0.2.2`），通过 **hostfwd** 端口映射互达：
```
caller: hostfwd udp::16062-:15062(SIP), 4000-:4000(RTP), 4001-:4001(RTCP)
callee: hostfwd udp::15062-:15062(SIP), 4002-:4002(RTP), 4003-:4003(RTCP)
```
拨号目标 `sip:user@10.0.2.2:<对端 hostfwd SIP 端口>`；SDP 用 `10.0.2.2:外部端口`（对端能达）。

### B. 与宿主互通（PJ_HOST_CALL）
宿主跑 `pjsua 2.17`（`works/tools/run_pjsua_uas.ps1`）做被叫。slirp 可达性要点：
- guest→host：用 `10.0.2.2:<port>`（网关 = 宿主回环别名）**直连**；
- host→guest：宿主进程**无法路由到** guest，唯一通道是 **hostfwd**（宿主发 `127.0.0.1:<hostfwd端口>`）。
- 因此需要**双侧地址重写**：guest 的 SDP offer/Contact 用 `127.0.0.1:<hostfwd端口>`；对端 SDP 重写为 `10.0.2.2`。
- **QEMU UDP hostfwd 是惰性关联**：必须让 guest **绑定自己的 4000/4001 为源端口**向网关发 dummy（`activate_hostfwd_rx()`），hostfwd 才会把宿主 RTP 转发给 guest:4000。
- **ACK**：宿主发布的 Contact 是 LAN IP（guest 不可达），pjsip 缓存的 ACK 地址改不动，故用**手动 ACK**（`pjsip_msg_print` + 原始 UDP 直发 `10.0.2.2:5060`）。

## 流程（信令 + 媒体）
1. `pj_init` → endpoint → tsx/UA/inv/100rel/timer 模块。
2. UDP transport 绑定 guest `:15062`，发布地址按拓扑选择（A：`10.0.2.2:外部端口`；B：`127.0.0.1:外部端口`）。
3. 构造 SDP（`create_audio_sdp`：PCMU + telephone-event + `a=ptime:10`）。
4. caller `uac_dial`（dialog → inv_uac → invite）；callee/UAS 模块应答 200。
5. CONFIRMED 后：mic 采集 → `pjmedia_stream`（`pjmedia_transport_udp` + `stream_create` + `pjmedia_clock` 10ms）→ RTP 双向。
6. **关键配置**：`frm_per_pkt=1`（10ms 包匹配 10ms 时钟）、`vad=0`、`jb_init=80/jb_min_pre=80`。
7. 统计 empty/normal/missing、DTMF 收发。

## 学到什么
- 把 01~08 的每一层在**真实网络 + 真实对端**上贯通。
- **slirp/NAT 下的互通规律**（对开发 QEMU 网络实验极重要）：
  - guest→host = 网关 `10.0.2.2:<port>`；
  - host→guest = 只能 hostfwd（发 `127.0.0.1:<hostfwd端口>`）；
  - UDP hostfwd 惰性关联需 guest 绑定源端口激活；
  - 双侧 SDP/Contact 地址重写。
- 根因排查方法论：丢包/静音 → 逐层 A/B 定位（网络→传输→流→编解码参数）。
- 手动 ACK、手动地址重写的"救场"技巧（pjsip 内部地址缓存深）。

## 如何启动
```powershell
# A. 双 QEMU
cmake -B build-caller ... -DPJ_DUAL_ROLE=caller ; cmake --build build-caller
cmake -B build-callee ... -DPJ_DUAL_ROLE=callee ; cmake --build build-callee
# 启动脚本：works/tools/run_dual_slirp.ps1（10s 通话，双侧喂 WAV）

# B. 与宿主互通
cmake -B build-hostcall ... -DPJ_DUAL_ROLE=caller -DPJ_HOST_CALL=ON ; cmake --build build-hostcall
# 宿主 pjsua：works/tools/run_pjsua_uas.ps1（--auto-answer 200 --null-audio --play-file=<10s.wav>）
# 整体测试：works/tools/run_hostcall_test.ps1
```

## 成功标准
- 双 QEMU：两侧串口 `media ALL PASSED`（20 轮回归零丢）。
- 与宿主：guest 串口 `media ALL PASSED` + 宿主 pjsua `Call 0 CONFIRMED`、收到 RTP/DTMF。
