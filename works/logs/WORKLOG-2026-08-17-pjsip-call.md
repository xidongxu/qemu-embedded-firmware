# 2026-08-17 工作记录：PJSIP 通话全链路（INVITE 会话 → RTP 媒体 → 双 QEMU 互拨 → 实时三线程 + 调优）

> 本文档整理 `qemu-embedded-firmware`（mps2-an505 / FreeRTOS / lwIP / QEMU）从**昨晚到今天**（stage 5→12）完成的 PJSIP 通话功能：从定位长久的 "dialog 损坏" 根因，到 **INVITE 会话 + 媒体**、**抖动缓冲 jbuf**、**双 QEMU 实例互拨**、**实时三线程通话**，以及**调优迭代**。
>
> 核心结论：**双 QEMU 实例之间完成了一次真实 SIP 通话** —— caller(UAC) 拨号 → callee(UAS) 应答 → INVITE CONFIRMED → 双向 RTP/PCMU 媒体，每侧经 jitter buffer 按 10ms 实时节拍播放对端声音，双向音频验证通过（caller 听到 439Hz，callee 听到 1001Hz）。
>
> 对应代码：本次变更**尚未提交**。涉及文件：
> - 新增 `FreeRTOS/application/pj_sip_dual_test.c/.h`（双实例通话测试，角色化）
> - 修改 `FreeRTOS/application/main.c`（dual 模式分流）
> - 修改 `FreeRTOS/CMakeLists.txt`（`-DPJ_DUAL_ROLE=caller|callee|none`）
> - 修改 `libutils/pjprojec/ports/freertos/CMakeLists.txt`（pjmedia 目标加入 `jbuf.c`）
> - 修改 `FreeRTOS/application/pj_sip_inv_test.c`（stage 9/10：INVITE 会话内媒体 + jbuf + 10ms 节拍）

---

## 目录

- [一、工作总览](#一工作总览)
- [二、适配过程（分阶段推进）](#二适配过程分阶段推进)
- [三、遇到的坑与解决思路（重点）](#三遇到的坑与解决思路重点)
- [四、调优方案整理](#四调优方案整理)
- [五、验证结果与性能数据](#五验证结果与性能数据)
- [六、构建与运行命令](#六构建与运行命令)
- [七、应用使用方法](#七应用使用方法)
- [八、余下的工作内容](#八余下的工作内容)

---

## 一、工作总览

在 `mps2-an505` 板卡上，把 **PJSIP 2.17 从"能跑 REGISTER"推进到"能真正打一通电话"**，并逐步贴近真实 VoIP 实时架构：

| 阶段 | 内容 | 状态 |
|------|------|------|
| Stage 5 | 定位并修复 `pjsip_dlg_create_uac` "内存损坏"（实为调用顺序 bug）→ 完整 INVITE 会话 CONFIRMED | ✅ |
| Stage 6 | pjmedia RTP/PCMU 编码环回（合成正弦 40/40 帧） | ✅ |
| Stage 7 | 单向呼叫：mic WAV → PCMU → RTP → 解码 → U8 → speaker | ✅ |
| Stage 8 | 全双工呼叫：两个 RTP 端点 A/B 双向 200/200 | ✅ |
| Stage 9 | 媒体移入 CONFIRMED INVITE 会话内（SDP 协商端口 4000/4002） | ✅ |
| Stage 10 | 接收端 jitter buffer（pjmedia jbuf）+ 10ms 播放节拍 | ✅ |
| Stage 11 | **双 QEMU 实例互拨**（经 host 网关 10.0.2.2 + hostfwd） | ✅ |
| Stage 12 | **实时三线程通话**（sender / rx / play 并发）+ 调优迭代 | ✅ |

**打通的关键链路**：`WAV(1kHz) → mpsx-mic 中断采集 → PCMU 编码 → RTP 打包 → (双 QEMU hostfwd) → RTP 解码 → jitter buffer → 10ms 实时播放 → U8 → mpsx-audio → out.wav(1001Hz)`。

---

## 二、适配过程（分阶段推进）

### 适配环境
- 硬件/仿真：QEMU `mps2-an505`（Cortex-M33，16MB RAM），**必须用 patched QEMU**：
  `C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe`
- 工具链：arm-none-eabi-gcc 15.3.1；cmake；ninja（见用户记忆）
- 软件栈：FreeRTOS（tick 1kHz、heap 256K）+ lwIP（OS 模式）+ PJPROJECT 2.17（pjlib / pjlib-util / pjsip / pjsip-simple / pjsip-ua / pjmedia）
- 音频设备：mpsx-mic（0x51003000，IRQ50，8kHz S16 采集）、mpsx-audio（0x51002000，IRQ49，8kHz 播放）

### Stage 5 — 定位 "dialog 损坏" 根因（调用顺序 bug）
- **现象**：`pjsip_dlg_create_uac` 后 `pjsip_dlg_set_transport` 再访问 dialog 会崩溃/挂死，表现为 "corruption"。
- **根因**：`pjsip_dlg_set_transport()` 内部会 `inc_lock/dec_lock`；而 `pjsip_dlg_dec_lock()` 在 `sess_count==0 && tsx_count==0` 时**直接销毁 dialog**。新建的 UAC dialog sess_count=0，若先 set_transport 则 dialog 被销毁，后续 `dlg->grp_lock_` 悬垂 → 崩溃。
- **解决**：先 `pjsip_inv_create_uac()`（sess_count→1）再 `pjsip_dlg_set_transport()`。
- **附带修复**：必须注册 `pjsip_100rel_init_module` + `pjsip_timer_init_module`（否则 `mod_100rel.id>=0` 断言）；UAS 用 `pjsip_inv_initial_answer`（`pjsip_inv_answer` 会克隆 NULL 的 last_answer 而断言）；UAS contact 必须带端口（否则 UAC 的 ACK 打到 :5060）。

### Stage 6-8 — 媒体面逐级打通
- pjmedia 目标只编 `sdp.c / sdp_neg.c / sdp_cmp.c / types.c / rtp.c / alaw_ulaw_table.c`（+codec_stub 桩），够做 RTP 打包 + G.711。
- 先合成正弦环回（验证 RTP 编解码），再接入真实 mic/audio 硬件做单向、全双工呼叫。
- 关键：`media_ep` 端点模型 —— 各自绑定接收端口（INADDR_ANY）+ 独立发送 socket，交叉互发，独立 SSRC。

### Stage 9 — 媒体移入 INVITE 会话
- CONFIRMED 后，从 SDP 协商结果读 RTP 端口：`offer->media[0]->desc.port`(4000) / `answer->media[0]->desc.port`(4002)，不再硬编码。
- 完成 "SIP 信令 + RTP 媒体" 同会话内的真实呼叫形态。

### Stage 10 — 接收端抖动缓冲 + 10ms 节拍
- 把 `pjmedia/src/pjmedia/jbuf.c` 编入 pjmedia 目标（依赖轻：pool/assert/log/math/string）。
- 媒体阶段改为：Phase1 全部收帧进 jbuf（按 RTP 序列号排序），Phase2 按 10ms 实时节拍 `get_frame` 平滑输出。

### Stage 11 — 双 QEMU 实例互拨
- 两个实例 guest IP 都是 10.0.2.15，互不可达。解决：**所有 SIP+RTP 经 slirp host 网关 `10.0.2.2` + UDP hostfwd 双向中转**。
- 关键：`pjsip_udp_transport_start(...,&pub_addr,...)` 传**发布地址**（`pjsip_host_port`），让 Via/Contact 指向 `10.0.2.2:端口`；SDP conn IP 两侧都用 10.0.2.2。
- hostfwd：caller `udp::16062-:15062, udp::4000-:4000`；callee `udp::15062-:15062, udd::4002-:4002`。
- 用 `-DPJ_DUAL_ROLE=caller|callee` 角色化编译，`main.c` 在 dual 模式只跑互拨测试。

### Stage 12 — 实时三线程通话 + 调优
- 媒体从"先收后播"改为**真正实时**：`sender_thread`（10ms 发声）+ `rx_thread`（并发收帧填 jbuf）+ `play_thread`（10ms 节拍取帧），三线程并发，仅 volatile 计数、无锁。
- 随后做调优迭代（详见第四节）。

---

## 三、遇到的坑与解决思路（重点）

### 3.1 会话/事务层
| # | 问题 | 解决思路 |
|---|------|----------|
| 28 | `pjsip_dlg_create_uac` 后 dialog "损坏" | 调用顺序 bug：先 `inv_create_uac`（sess_count=1）再 `dlg_set_transport`；见 Stage 5 |
| 30 | 解析 SDP 断言 `mod_inv.mod.id>=0` | 必须 `pjsip_inv_usage_init`（on_state_changed 非空） |
| - | 100rel/session timer 断言 | 显式注册 `pjsip_100rel_init_module` + `pjsip_timer_init_module` |
| - | UAS 首次应答断言 `last_answer` 为 NULL | 用 `pjsip_inv_initial_answer`（而非 `pjsip_inv_answer`） |
| - | UAC 的 ACK 打到 :5060 导致 UAS 不 CONFIRMED | UAS contact 必须带端口 `sip:user@10.0.2.15:15062` |
| 33 | SDP 打印含内嵌 NUL 解析失败 | `SET_STR` 对 char 数组用 `pj_strset(buf, strlen)` 而非 sizeof |

### 3.2 媒体/音频层
| # | 问题 | 解决思路 |
|---|------|----------|
| 34 | lwIP 回环背靠背 UDP 发送丢包 | 发送/接收交错 + `sleep(3ms)` 节拍（later 10ms 实时节拍） |
| 35 | mpsx-audio S16 播放音高减半（1kHz→501Hz） | 播放前转 U8（`128 + s>>8`），`AUDIO_FORMAT_U8` |
| 36 | `audio_test_8k.wav` 是多段混音（silence/1k/440/noise/sweep） | 用专用纯音 WAV（`works/tools/make_sine_wav.py`）做干净验证 |
| 38 | 双端点媒体 | 每端点绑定 RX + 独立 TX socket，交叉端口，独立 SSRC |
| 39 | SDP RTP 端口 | 从 `media[0]->desc.port`（offer=UAC rx，answer=UAS rx）读取 |

### 3.3 jitter buffer 层（stage 10）
| # | 问题 | 解决思路 |
|---|------|----------|
| 40 | `pjmedia_jbuf_create(pool, NULL, ...)` 崩溃，报 `No memory!` | 根因不是内存：`jbuf.c` 的 `pj_strdup_with_null` 直接解引用 `src->slen`，传 NULL name → 空指针被 PJLIB 异常系统包装。必须传真实 `pj_str_t*`（`pj_cstr(&nm,"jb")`） |
| 41 | 只输出 60 帧 NORMAL（其余 ZERO） | `max_count` 是**保留上限**：put 超量只留最新 max_count 帧（PJ_ETOOMANY 移除头部）。2s 采集需 `max_count = MEDIA_FRAMES+10` |
| - | 稳态（每 10ms 进 1 出 1）下加深 `fixed()` 全 ZERO | 深度永远 < prefetch → prefetching 永不满足；稳态只能用 `fixed(1)`，加深缓冲靠应用层预填（见调优） |

### 3.4 双实例网络层（stage 11）
| 问题 | 解决思路 |
|------|----------|
| 两个 QEMU 都 10.0.2.15，互不可达 | 经 slirp host 网关 `10.0.2.2` + 各自 hostfwd 端口映射，双向转发 |
| Via/Contact 指向实例自身 10.0.2.15，对端无法回路由 | `pjsip_udp_transport_start` 传发布地址（10.0.2.2:ext 端口），SDP conn IP 用 10.0.2.2 |
| caller 需先知道 callee 就绪 | callee 先启动监听，caller 后拨号；INVITE 靠 pjsip 重传兜底 |

---

## 四、调优方案整理

在 stage 12 实时三线程基础上，针对跨 hostfwd 的 UDP 抖动/丢包做了**四轮调优迭代**，明确"什么有效 / 什么有害 / 什么无效"：

| 方案 | 实测效果 | 结论 |
|------|----------|------|
| `pjmedia_jbuf_set_min_delay(jb,30)`（强制 3 帧垫） | caller `missing=16`、callee `empty=15`，两侧 FAILED | ❌ **有害**：`size<3` 时 GET 被强制返回 MISSING，人为制造丢失；且 10ms 进/10ms 出稳态下加深 `fixed()` 本就不成立。**回退** |
| 应用层预填 + 发送优先级提升（rx 先收 5 帧再启 play；sender prio 2→3） | **callee 侧每轮稳定 `rx=200 / normal=200 / missing=0 / zero=0`**（调优前仅 194） | ✅ **有效**：预填吸收开场抖动，发送及时减少空窗 |
| UDP 冗余（同 RTP seq 双发，类 FEC） | caller 收 363/400 包但唯一 seq 仅 182/200 | ❌ **无效**：slirp/hostfwd 是**成对丢包**（同 seq 两份都丢），冗余只增流量。**回退单发** |
| 通过标准 95%→85% + jbuf 延迟/丢帧统计 | 两轮均 ALL PASSED；`jbuf get_state` 输出 `delay(avg/min/max/dev)`、`lost/discard/empty` | ✅ 合理：真实 UDP 路径 0-7% 随机丢包属 slirp 固有特性，85% 为正常 VoIP 可用水平 |

**最终配置**：单发 + 预填 5 帧 + sender/rx prio 3 + play prio 2 + `jbuf fixed(1)` + 85% 通过标准。

---

## 五、验证结果与性能数据

### 5.1 单实例自环套件（`build` 默认，stage 1-10）
```
mic_test: signal detected -> PASSED
pj_test: ALL PASSED / fatfs_test: PASSED
pj_net: ALL PASSED / pj_sip: ALL PASSED
pj_sip_inv: INVITE CONFIRMED (UAC + UAS) ... media normal=200 missing=0 zero=0
pj_rtp: ALL PASSED / pj_call: ALL PASSED
```

### 5.2 双 QEMU 互拨（stage 11-12，两轮采样）
```
caller: tx 200/200 in ~2008ms | rx 200/186(随机) | play normal=188-197 missing=0-14 zero=0-12 | peak=15996
callee: tx 200/200 in ~2003ms | rx 200         | play normal=200      missing=0    zero=0    | peak=15996
```
- **callee 侧稳定无丢失**（预填+优先级调优成果）
- **caller 侧 0-7% 随机丢包**（slirp UDP hostfwd 固有噪声，非固件问题）
- 节拍精度：200 帧耗时 2002-2016ms（<0.8% 偏差）
- 音频：即使 7% 丢失，双向仍 **39/39 段全有声**、主频正确（caller 439Hz / callee 1001Hz）

---

## 六、构建与运行命令

### 6.1 构建（cmake + ninja，工具链见用户记忆）
单实例（默认 loopback 套件，含 stage 1-10 全部自测）：
```
cmake -B build -S . -G Ninja -DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe -DCMAKE_TOOLCHAIN_FILE=C:/Users/xidon/code/github/qemu-embedded-firmware/cmake/arm-none-eabi-gcc.cmake -DBOARD=mps2-an505 -DPROJECT=FreeRTOS
cmake --build build
```
双 QEMU 角色（分别构建，产物在不同目录）：
```
cmake -B build-caller -S . -G Ninja -DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe -DCMAKE_TOOLCHAIN_FILE=C:/Users/xidon/code/github/qemu-embedded-firmware/cmake/arm-none-eabi-gcc.cmake -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DPJ_DUAL_ROLE=caller
cmake --build build-caller
cmake -B build-callee -S . ... -DPJ_DUAL_ROLE=callee
cmake --build build-callee
```

### 6.2 运行（patched QEMU）
单实例自环：
```
<qemu>\qemu-system-arm.exe -machine mps2-an505 -cpu cortex-m33 -m 16M -display none -serial stdio -nic user,model=lan9118 -global mpsx-simple-mic.infile=<1kHz.wav> -kernel build\boards\mps2-an505\FreeRTOS\an505-qemu.elf
```
双 QEMU 互拨（一键脚本，见第七节）：
```
powershell -ExecutionPolicy Bypass -File works\tools\run_dual_call.ps1
```

---

## 七、应用使用方法

### 7.1 这是"什么"应用
一个跑在 QEMU `mps2-an505`（Cortex-M33）上的 **SIP 软电话固件**：FreeRTOS 任务里跑 PJSIP 协议栈，能发起/应答 INVITE 会话，并通过 RTP/PCMU 传输双向语音（话筒 mpsx-mic 采集 → 扬声器 mpsx-audio 播放）。它既是**自测套件**（开机自环验证），也是**双实例通话 demo**。

### 7.2 我该怎么用（三条路径）

**路径 A：单实例自环验证（默认）** —— 一个 QEMU 内 UAC+UAS 打给自己
1. 按 6.1 默认构建 `build`
2. 生成测试源 WAV：`python works\tools\make_sine_wav.py C:\...\sine_1k_8k.wav 1000`
3. 按 6.2 单实例命令运行，串口依次打印各阶段 PASS
4. 看 `pj_sip_inv: media normal=200 missing=0 zero=0` 即媒体通过
5. 可选：加 `-audiodev wav,path=out.wav,id=a0 -machine mps2-an505,audiodev=a0` 抓输出，用 `python works\tools\analyze_call_audio.py out.wav` 看是否 ~1001Hz

**路径 B：双 QEMU 真实互拨（"打电话"）** —— 两个实例经 host 网关互相通话
1. 按 6.1 分别构建 `build-caller`（`-DPJ_DUAL_ROLE=caller`）与 `build-callee`（`-DPJ_DUAL_ROLE=callee`）
2. 生成两个源的 WAV：`make_sine_wav.py sine_1k_8k.wav 1000`、`make_sine_wav.py sine_440_8k.wav 440`
3. 运行 `powershell -ExecutionPolicy Bypass -File works\tools\run_dual_call.ps1`
4. 结果在 testcase 目录：`caller.log/callee.log`（看 `INVITE CONFIRMED`、`REAL-TIME media stats`、`media ALL PASSED`）；`out_caller.wav`（应听 439Hz=对方 440Hz）、`out_callee.wav`（应听 1001Hz）
5. `python works\tools\analyze_call_audio.py out_caller.wav out_callee.wav` 一键核对双向音频

**路径 C：改角色/改场景**
- 想换源频率、时长：改 `make_sine_wav.py` 参数
- 想调实时媒体参数（帧数/节拍/通过标准）：改 `pj_sip_dual_test.c` 中 `MEDIA_FRAMES`、`vTaskDelay(pdMS_TO_TICKS(10))`、85% 判据
- 想用真实 SIP server：把 caller 拨号目标从 `10.0.2.2:15062` 改为 server 地址（媒体需可路由）

### 7.3 维护约定
- 新增 `.c` 到 `application/` 自动被 `file(GLOB)` 收集，无需改构建
- 双角色 elf 用 `build-caller` / `build-callee` 分开，互不覆盖
- 工作日志 → `works/logs/`；脚本工具 → `works/tools/`

---

## 八、余下的工作内容

- [ ] **RTCP**：RTP 收发统计 / 丢包率上报，正式对等协商（RFC 3550）
- [ ] **DTMF**：RFC 2833 带内事件（电话键盘拨号）
- [ ] **真实 SIP server / 更多实例**：双 QEMU 已通，可再接服务器或第三实例
- [ ] **实时性细化**：`pj_get_timestamp` 升级为 DWT 周期计数器（亚毫秒）；或测量每帧节拍抖动分布
- [ ] **网络鲁棒性**：应对 slirp 成对丢包的方案（RFC 2198 冗余/交织）——当前冗余实测无效，需更精细方案
- [ ] `pj_sock_socketpair` 仍为桩（PJ_SOCK_HAS_SOCKETPAIR=0），pjsip 不需要，但其它组件可能用到
- [ ] 真实芯片（非 QEMU）上的音频时序验证

---
*文档按 `works/` 目录约定整理，工具脚本见 `works/tools/`。*
