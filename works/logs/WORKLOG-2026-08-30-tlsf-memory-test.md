# WORKLOG-2026-08-30：TLSF 内存占用与泄露测试

> **场景**：工程已接入统一 TLSF 内存分配器（`libmem/tlsf`，见
> `WORKLOG-2026-08-30-tlsf-malloc.md`）——C `malloc`、C++ `new/delete`、
> FreeRTOS 内核 `pvPortMalloc` 全部走同一个 TLSF 池。
> 本文档描述**如何测试系统内存占用、如何判断是否有内存泄露**，供后续回归参考。

---

## 1. 背景与目的

统一内存管理之后，需要一种手段**量化系统内存占用**并**验证无内存泄露**。
做法：在 guest UDP 命令服务器新增 `mem` 命令，直接读取 TLSF 池的实时统计
（总池 / 已用 / 剩余 / 历史最低剩余），配合"多次通话-挂断"循环观察指标是否稳定。

---

## 2. 测试原理

### 2.1 TLSF 池统计指标（`libmem/tlsf/ports/freertos/tlsf_port.c`）

| 指标 | 来源 | 含义 |
|---|---|---|
| `pool` 总池 | `tlsf_port_get_total_size()` | 初始化时分配给 TLSF 的连续内存区 `[_end, _estack - _Min_Stack_Size)` 大小 |
| `used` 已用 | `tlsf_port_get_used_size()` | 遍历 pool 求和所有**已用块**大小（不含分配器开销） |
| `free` 剩余 | `tlsf_port_get_free_size()` | 遍历 pool 求和所有**空闲块**大小 |
| `minfree` 历史最低剩余 | `tlsf_port_get_min_free_size()` | 每次统计时更新 low-water mark |

> 统计方式：`tlsf_walk_pool()` 遍历整个 pool，`O(块数)`，仅适合**低频查询**
> （如人机/命令触发），不适合每次 malloc 都统计。

### 2.2 如何判断泄露

内存泄露的本质是：**分配后无法释放，可用内存单调减少**。据此：

- **`minfree` 是否持续下降** —— 泄露的最强信号。若无泄露，系统在反复
  分配-释放后，`minfree` 会在某个**稳定低点**停止下降（峰值占用不再增长）。
- **挂断后 `used` 是否回到基线** —— 每次通话结束，通话相关内存应释放；
  若 `used` 随轮次**逐轮抬高**，说明有东西没释放（泄露）。
- **`free` 是否持续减少** —— 若多次通话后 `free` 一路下滑，同样指向泄露。

> 注意：挂断后 `used` 比冷启动基线高几十~上百 KB 是**正常的**（pjlib 池缓存、
> 注册保活、TCP 连接等长期驻留，且之后**不再增长**）——区分"一次性驻留"和
> "随轮次增长"是关键。

---

## 3. 环境准备

- **QEMU**：`qemu-system-arm.exe`（mps2-an505, cortex-m33, 16MB RAM），tap0 模式
- **FreeSWITCH**：`FreeSwitchConsole.exe` 手动运行，`internal-lo` profile 绑定 172.16.23.1
- **固件**：`build-phone\boards\mps2-an505\FreeRTOS\an505-qemu.elf`（`TLSF_MALLOC=ON`）
- **网络**：guest 172.16.23.50 ↔ 宿主 172.16.23.1（tap0，零 NAT）
- **guest UDP 命令服务器**：端口 15000

启动 QEMU：
```powershell
& 'C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe' `
  -machine mps2-an505 -cpu cortex-m33 -m 16M `
  -kernel '<repo>\build-phone\boards\mps2-an505\FreeRTOS\an505-qemu.elf' `
  -display none -serial file:<repo>\works\tools\tap0-tlsf-serial.log `
  -nic tap,ifname=tap0,script=no,downscript=no
```

---

## 4. 测试方法（`mem` 命令）

guest UDP 命令服务器新增命令：

```
mem → tlsf pool=<total> used=<used> free=<free> minfree=<minfree>
```

实现位置：`boards/mps2-an505/FreeRTOS/application/phone_net.c`（`pnet_exec` 里
`memp` 分支之后新增 `mem` 分支，调用 `tlsf_port_get_*`）。
头文件：`libmem/tlsf/ports/freertos/tlsf_port.h`（include 路径已由 tlsf 库 PUBLIC 暴露）。

---

## 5. 测试步骤（可复现脚本）

### 5.1 单次查看（基线）

```powershell
# 等 guest 起来（~15s）后：
$u=New-Object Net.Sockets.UdpClient; $u.Client.ReceiveTimeout=3000
$b=[Text.Encoding]::ASCII.GetBytes('mem')
$u.Send($b,$b.Length,'172.16.23.50',15000)|Out-Null
$e=New-Object Net.IPEndPoint([Net.IPAddress]::Any,0)
$r=$u.Receive([ref]$e); [Text.Encoding]::ASCII.GetString($r)
```

### 5.2 多轮通话压力测试（推荐 5~10 轮）

```powershell
function Udp-Cmd($cmd){
  $u=New-Object Net.Sockets.UdpClient; $u.Client.ReceiveTimeout=3000
  $b=[Text.Encoding]::ASCII.GetBytes($cmd)
  $u.Send($b,$b.Length,'172.16.23.50',15000)|Out-Null
  try{$e=New-Object Net.IPEndPoint([Net.IPAddress]::Any,0); $r=$u.Receive([ref]$e)
      [Text.Encoding]::ASCII.GetString($r)}catch{'timeout'}
  $u.Close()
}

# 基线
Write-Host ('idle : '+(Udp-Cmd 'mem'))

# 5 轮通话：每轮 通话3s → 记录通话中mem → 挂断 → 记录挂断后mem
for($i=1;$i -le 5;$i++){
  Udp-Cmd 'dial 9196' | Out-Null
  Start-Sleep -Seconds 3
  $m1 = Udp-Cmd 'mem'            # 通话中（峰值附近）
  Udp-Cmd 'hangup' | Out-Null
  Start-Sleep -Seconds 1
  $m2 = Udp-Cmd 'mem'            # 挂断后（应回落）
  Write-Host ("[$i] in-call: $m1 | after: $m2")
}
```

---

## 6. 实测数据（2026-08-30，QEMU + FS 1.11.3）

```
[0] idle   : tlsf pool=14639808 used=433856 free=14201148 minfree=14201148
[1] in-call: used=561384 free=14073276 | after: used=488344 free=14146468 minfree=14073276
[2] in-call: used=611972 free=14022484 | after: used=520536 free=14114160 minfree=14022484
[3] in-call: used=644184 free=13990172 | after: used=520540 free=14114084 minfree=13990172
[4] in-call: used=625908 free=14008508 | after: used=534436 free=14100188 minfree=13990172
[5] in-call: used=625880 free=14008516 | after: used=502244 free=14132500 minfree=13990172
```

**汇总**（字节）：

| 指标 | 值 |
|---|---|
| TLSF 总池 | 14,639,808（~14.6 MB，主堆区全部分配给 TLSF） |
| 冷启动基线 used | ~433,856（~424 KB） |
| 单通通话峰值 used | ~626K–644K |
| 挂断后 used（稳定区间） | ~488K–535K |
| minfree 稳定点 | 13,990,172（第 3 轮后不再下降） |

---

## 7. 结果分析

**结论：无内存泄露。**

1. **`minfree` 第 3 轮后恒定在 13,990,172** —— 峰值内存占用不再增长。
   若有泄露，`free` 会持续减少、`minfree` 会一路走低。
2. **通话峰值稳定**（used 峰值 ~626K–644K，第 4/5 轮不上升）。
3. **挂断后回落稳定**（~488K–535K 区间波动，无逐轮抬高趋势）。
4. **`free` 稳定**在 14.0–14.13 MB 区间波动。

> 挂断后比冷启动基线高 ~60–100 KB：pjlib 池缓存 / 注册保活 / 连接等
> **长期驻留**，属于一次性开销，之后不再增长，非泄露。

---

## 8. 注意事项 / 后续

- `mem` 统计走 `tlsf_walk_pool`（O(块数)），**仅用于低频调试**，勿高频调用。
- 若后续观察到 `minfree` 持续下降，可结合串口日志/`memp`（lwIP 池）定位
  是 TLSF 侧泄露还是 lwIP 池耗尽。
- 当前系统占用极低（idle ~424KB / 通话峰值 ~644KB / 池 14.6MB），
  内存余量充足，为后续 SRTP/TLS（需额外内存）留了很大空间。
- 本测试可作回归基线：每次改动后跑 5.2 的 5 轮脚本，`minfree` 不持续下降即视为通过。

---

## 9. 相关文档

- `WORKLOG-2026-08-30-tlsf-malloc.md` —— TLSF 集成方案/文件清单
- `WORKLOG-2026-08-29-smoke-regression.md` —— 冒烟回归（含通话/媒体/memp 检查）
