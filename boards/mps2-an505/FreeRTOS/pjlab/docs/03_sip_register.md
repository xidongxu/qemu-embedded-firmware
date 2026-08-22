# 实验 03 — SIP REGISTER 注册（pj_sip_test.c）

**阶段**：PJSIP 栈 + REGISTER 事务（stage 3）。

## 目的
验证 **PJSIP 信令栈**（`pjsip_endpoint`、UDP transport、`pjsip_regc` 注册客户端、事务层）在 FreeRTOS + lwIP 上能启动并完成一次**完整事务往返**。

## 思路
把 REGISTER 发给**板子自己**（UDP 回环）：本机起一个模块自动回 `200 OK`，REGISTER 客户端收到后走完整个事务状态机，无需外部 SIP 服务器。

## 流程
1. `pjsip_endpt_create` 建 endpoint；`pjsip_tsx_layer_init_module` 初始化事务层。
2. `pjsip_udp_transport_start` 绑定本机 `:15060`。
3. 注册一个**自定义模块**：收到 REGISTER 请求时自动回 `200 OK`。
4. `pjsip_regc_create/init/register` 发起 REGISTER（发到本机 `:15060`）。
5. 事件循环 `pjsip_endpt_handle_events` 驱动事务（客户端 tx → 服务器 tx → 200 → 回调）。
6. 校验回调返回 `200 OK`。

## 学到什么
- PJSIP 的**层次**：endpoint → transport → transaction(tsx) → 模块(module)。
- **事务层（tsx）**：状态机（Calling/Proceeding/Completed/Terminated）、重传、超时——SIP 可靠性的核心。
- **模块机制**：`pjsip_module` 是 PJSIP 的扩展点（on_rx_request/on_rx_response），后续 UAS 应答、我们的地址重写都靠它。
- 事件循环 `pjsip_endpt_handle_events` 驱动一切（嵌入式上无阻塞收包）。

## 如何启动
默认构建（`pj_sip_test_run()` 在 02 之后执行）。

## 成功标准
串口打印 `pj_sip: PASSED`（REGISTER 事务 200 OK 往返成功）。
