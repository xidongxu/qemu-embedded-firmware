# 实验 02 — 网络 socket + ioqueue（pj_net_test.c）

**阶段**：PJLIB socket / ioqueue 在 lwIP 上的验证。

## 目的
验证 pjlib 的网络抽象（`pj_sock_*`、`pj_ioqueue_*`、`pj_sock_select`）在 **lwIP** 上可用。这是 SIP/RTP 走网络的基础（pjmedia transport、pjsip transport 都基于它）。

## 思路
在**板子自己 IP 上做 UDP 回环**：客户端 socket 发包到服务器 socket（同一块板的 IP，lwIP 把发往本机地址的包回环到本地 socket），服务器回包，无需外部对端。

## 流程
1. `pj_sock_socket(AF_INET, SOCK_DGRAM)` 建客户端与服务器 socket。
2. `pj_gethostip()` 拿到板子接口 IP。
3. 服务器 `pj_sock_bind` 到本机 `:15000`。
4. **同步路径**：`pj_sock_sendto` → `pj_sock_recvfrom` 回环收发。
5. **异步路径**：`pj_ioqueue_create` + `pj_ioqueue_register_sock` + `pj_ioqueue_recvfrom/sendto`（注册回调，`pj_ioqueue_poll` 驱动）。
6. `pj_sock_select` 做就绪检查。
7. 校验收发的字节内容一致。

## 学到什么
- pjlib 的 socket API 与 lwIP socket 的对应关系。
- **ioqueue（异步 IO 事件循环）**：pjproject 用单线程 `pj_ioqueue_poll` 驱动所有 socket，这是嵌入式上不依赖多线程 IO 的关键机制（后续媒体 transport 就靠它收 RTP/RTCP）。
- `pj_ioqueue_poll` 的"每轮最多处理 N 个 IO、防止发送/播放任务饿死"的思路（见源码 ioq 注释）。

## 如何启动
默认构建（`pj_net_test_run()` 在 01 之后执行）。

## 成功标准
串口打印 `pj_net: PASSED`。
