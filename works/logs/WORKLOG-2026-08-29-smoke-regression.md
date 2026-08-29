# WORKLOG-2026-08-29：QEMU 电话冒烟回归测试（run_smoke_regression.ps1）

> 平台：QEMU mps2-an505 + Cortex-M33 + FreeRTOS + PJSUA(pjproject 2.17) + FreeSWITCH
> 网络：tap0 独立网段（guest 172.16.23.50 ↔ 宿主 172.16.23.1）
> 脚本：`works/tools/run_smoke_regression.ps1`

---

## 1. 背景与目的

在 120105（第二次通话失败）修复后，需要一套**可重复、可自动化的回归手段**，
验证"注册 → 拨号 → 双向媒体 → DTMF → 挂断"这条核心电话链路长期稳定、
且**不产生资源泄漏**。本脚本即为此编写，是 M0（基线固化 / 自动化回归）的一部分。

关联修复：
- `libutils/lwip/ports/lwipopts.h`：`MEMP_NUM_UDP_PCB 8→24`、`MEMP_NUM_NETCONN 12→24`
  （非泄漏，是池太小——每次通话占 ~4 个 UDP_PCB，系统 ~2，第二次通话 10>8 报 ENOBUFS）
- `boards/.../application/phone_net.c`：新增 `memp` 命令（打印 lwIP 各内存池用量），
  供本脚本做资源泄漏检查。

---

## 2. 测试逻辑（脚本做的事）

脚本通过 guest 的 **UDP 命令服务器（端口 15000）** 驱动电话，每次迭代完整走一遍：

```
预检查：
  1. guest 可达（发 status）→ 不可达则 FATAL 退出
  2. 若已有通话则先 hangup（保证从 IDLE 开始）
  3. 记录基线 memp 的 UDP_PCB 用量（$baseUdp）

每次迭代：
  1. dial <Number>           拨号（默认 9196 echo）
  2. 轮询 status 直到 call=ACTIVE（每 800ms，超时 $CallTimeoutS=12s）
  3. 媒体检查：连续两次 stat，rx/tx 必须递增（证明双向 RTP 流动）
     （MediaWaitS=3s 后采一次，再隔 2s 采第二次）
  4. dtmf 1234               发送 RFC2833 DTMF，须 rc=0
  5. hangup                  挂断，须回到 IDLE
  6. 资源检查：查 memp，UDP_PCB used 不得超过 基线+2
     （超过则判定泄漏 → FAIL）
  7. 记录本次耗时

汇总：
  PASS/FAIL 计数、总耗时、退出码（0=全过，1=有失败）
```

### 判定标准（每项）
| 步骤 | 通过条件 |
|---|---|
| dial | 响应含 `rc=0` |
| call | 超时内 `call=ACTIVE` |
| media | 两次 `stat` 的 rx、tx 均增长，且 `loss=0` |
| dtmf | 响应含 `rc=0` |
| hangup | 之后 `call=IDLE` |
| 资源 | `memp` 的 `UDP_PCB used <= 基线+2` |

---

## 3. 使用方法

```powershell
# 基础冒烟（3 次）
powershell -ExecutionPolicy Bypass -File works\tools\run_smoke_regression.ps1

# 长测试 / soak 基础（20 次 ≈ 2.3 分钟）
powershell -ExecutionPolicy Bypass -File works\tools\run_smoke_regression.ps1 -Iterations 20

# 打到其他分机（如手机 1005）
powershell -ExecutionPolicy Bypass -File works\tools\run_smoke_regression.ps1 -Number 1005
```

### 参数
| 参数 | 默认 | 说明 |
|---|---|---|
| `GuestIp` | `172.16.23.50` | guest 地址 |
| `Port` | `15000` | guest UDP 命令服务器端口 |
| `Number` | `9196` | 拨号目标（FS echo 测试号 / 其他分机） |
| `Iterations` | `3` | 迭代次数 |
| `CallTimeoutS` | `12` | 等待 call=ACTIVE 的超时（秒） |
| `MediaWaitS` | `3` | 媒体检查前等待（秒） |

### 前置条件
- QEMU guest 已启动、FreeSWITCH 已运行（echo 9196 在 default dialplan）
- guest 已注册到 FS（`1000@192.168.23.7`）
- tap0 网络正常；FreeSWITCH `internal-lo` 监听 `172.16.23.1`

---

## 4. 测试效果（实测数据，2026-08-29）

### 20 次迭代长测试：**20/20 PASS，耗时 137.2s（2.3 分钟）**

```
==== SUMMARY ====
PASS: 20   FAIL: 0   elapsed: 137.2s (2.3 min)
ALL PASSED
```

### 关键观察
| 指标 | 结果 |
|---|---|
| 每次迭代耗时 | ~6.9s（稳定） |
| 通话建立 | 全部 `call=ACTIVE`（9196 echo 应答 <4s） |
| 双向媒体 | 每次 rx/tx 递增 ~100 包，**loss=0** |
| DTMF | 全部 `rc=0`（RFC2833 发送成功） |
| 挂断 | 全部回到 `IDLE` |
| **资源泄漏** | **UDP_PCB 全程恒定 `2/24`**（基线 2，20 次后仍 2）——无泄漏 |
| UDP 收发总量 | xmit 2509→8006、recv 2418→7712（持续正常） |

**结论**：120105 修复后，连续 20 次完整通话（拨号/媒体/DTMF/挂断）零失败、
零资源泄漏、媒体零丢包。系统满足 M0 的"可连续通话、可重复验证"基线要求。

---

## 5. 后续扩展（soak 方向）

- **24h soak**：`-Iterations 1000`（≈2 小时）或脚本化长时间挂机/呼叫循环
- **多目标**：`-Number 1005` 打到真实手机，覆盖真实对端场景
- **网络异常**：配合 QEMU 抓包 / 模拟丢包，验证媒体断流检测与恢复
- **失败留档**：当前打印到 stdout，后续可加 `-LogFile` 落盘 + 失败时输出 guest/FS 日志

---

## 6. 相关文件

| 文件 | 说明 |
|---|---|
| `works/tools/run_smoke_regression.ps1` | 冒烟回归脚本（本文档主体） |
| `libutils/lwip/ports/lwipopts.h` | 120105 修复（UDP_PCB 8→24、NETCONN 12→24） |
| `boards/.../application/phone_net.c` | `memp` 命令（lwIP 池用量诊断） |
| `works/logs/WORKLOG-2026-08-29-industrial-voip-roadmap.md` | 工业级差距清单 / 路线图（M0） |
