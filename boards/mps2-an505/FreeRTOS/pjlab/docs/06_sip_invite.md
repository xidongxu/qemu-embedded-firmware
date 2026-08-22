# 实验 06 — SIP INVITE 会话 + SDP 协商（pj_sip_inv_test.c）

**阶段**：完整 INVITE 会话（stage 5）。

## 目的
在 03 的 REGISTER（无会话）之上，验证**完整 INVITE 会话**：UAC 发起呼叫、UAS 应答、双方完成 **SDP 协商**、会话进入 **CONFIRMED**（含自动 ACK）。这就是"建立一通话"的信令骨架。

## 思路（板内回环）
同一块板上同时跑 UAC 和 UAS：UAC 发 INVITE（带 SDP offer）到本机，UAS 模块应答 `200 OK`（带 SDP answer），双方协商后 CONFIRMED。

## 流程
1. **UAC 侧**：`pjsip_dlg_create_uac` 建对话框 → `pjsip_inv_create_uac`（创建 INVITE 会话 + SDP offer）→ `pjsip_dlg_set_transport` → `pjsip_inv_invite` + `pjsip_inv_send_msg`。
2. **UAS 侧**：自定义模块收到 INVITE → `pjsip_dlg_create_uas_and_inc_lock` → `pjsip_inv_create_uas` → `pjsip_inv_initial_answer(200, SDP answer)` → `pjsip_inv_send_msg`。
3. 双方进入 `PJSIP_INV_STATE_CONFIRMED`（pjsip 自动发 ACK）。
4. 校验状态回调。

> ⚠️ **关键坑（曾经的 dialog 损坏 bug）**：`pjsip_dlg_set_transport()` 内部会 `inc_lock/dec_lock`，而 `dec_lock` 在 `sess_count==0 && tsx_count==0` 时会**销毁对话框**。所以 UAC 必须**先** `pjsip_inv_create_uac`（把 sess_count 加 1）**再** `pjsip_dlg_set_transport()`。

## 学到什么
- **Dialog（对话）**：一对 UA 之间的端到端信令关系（Call-ID、From/To tag、CSeq 序列）。
- **Invite session（会话）**：状态机 NULL→CALLING→CONNECTING→CONFIRMED，以及自动 ACK。
- **SDP 协商（offer/answer）**：`pjmedia_sdp_neg`——协商编码、RTP 端口、ptime 等。`pjmedia_sdp_session` 结构（c=/m=/a= 行）在 09 里被我们手动改（地址重写）。
- **调用顺序陷阱**：对话框引用计数与生命周期管理（嵌入式上极易踩）。

## 如何启动
默认构建（`pj_sip_inv_test_run()` 在 05 之后执行）。

## 成功标准
串口打印 `pj_sip_inv: PASSED`（INVITE 会话 CONFIRMED）。
