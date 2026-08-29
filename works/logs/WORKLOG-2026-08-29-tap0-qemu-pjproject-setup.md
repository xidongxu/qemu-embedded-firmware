# WORKLOG-2026-08-29：Hyper-V Windows 下用 QEMU 开发 pjproject 的 tap0 网络方案（完整部署参考）

> **本文档用途**：在**启用了 Hyper-V 虚拟机监控程序的 Windows** 上，用 QEMU 跑 mps2-an505
> 嵌入式固件 + pjproject（pjsua 电话）对接本机 FreeSWITCH 的**完整、可复现**配置流程。
> 供新机器重新部署时照做。**所有内容均已在本机（2026-08-29）实测通过**。
>
> 一句话结论：**不要用 Windows「网络桥接」（Hyper-V 下不可用），改用 OpenVPN 提供的
> TAP 网卡 + 独立网段（172.16.23.0/24），guest 与宿主直连，零 NAT、零 pjproject 补丁。**

---

## 0. 为什么是这套方案（背景/踩坑结论）

| 方案 | 结论 | 原因 |
|---|---|---|
| QEMU `-nic user`（slirp）+ hostfwd | ❌ 放弃 | slirp 单向（guest 出、hostfwd 进）；`127.0.0.1` 在 guest/宿主两侧语义分裂 → guest 的 ACK 发回自身回环 → 32s 挂断；RTP 也要一堆补丁 |
| Windows「网络桥接」把 tap0 桥到以太网 | ❌ **不可用** | 宿主机 **`HypervisorPresent = True`**（Hyper-V 虚拟机监控程序运行）时，微软限制 Windows Network Bridge **不可用**，建桥直接报错失败 |
| **OpenVPN TAP 网卡 + 独立网段** | ✅ **采用** | tap0 是 L2 点对点虚拟网卡，guest 与宿主组 `172.16.23.0/24` 段直连；SIP/RTP 都走 UDP，双向直通；不需要网桥、NAT、或改 pjproject 源码 |

**架构图**：
```
┌───────────── Windows 宿主机（Hyper-V 运行中）─────────────┐
│                                                           │
│  QEMU mps2-an505 (guest)        FreeSWITCH 1.11.2          │
│  guest IP : 172.16.23.50/24     internal-lo profile        │
│  SIP port : 15062               sip-ip/rtp-ip=172.16.23.1  │
│  gateway  : 172.16.23.1         SIP port: 5060             │
│     │                               │                     │
│     └────── tap0 (172.16.23.1/24) ──┘    ← OpenVPN TAP 网卡 │
│                 L2 直连，双向 UDP                            │
└───────────────────────────────────────────────────────────┘
```

> `OpenVPN Data Channel Offload`（DCO）是 OpenVPN 另一个 **TUN 类型**虚拟网卡，闲置，
> **与桥接失败无关**（TUN 本就不能桥接）。本方案用的是 `tap0`（TAP-Windows Adapter V9）。

---

## 1. 前置依赖（必须安装）

| 组件 | 作用 | 本机路径（参考） |
|---|---|---|
| **OpenVPN** | 提供 TAP-Windows 驱动（创建 TAP 网卡，**需手动重命名连接名为 `tap0`**，见 2.1） | 装完即有 `TAP-Windows Adapter V9` 驱动 |
| **QEMU**（ARM 版） | 运行 mps2-an505 guest | `C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe` |
| **ARM GCC 工具链** | 编译固件 | `C:\Users\xidon\program\ARM_Gcc\gcc-arm-none-eabi-15.3-2026.06`（15.3.1） |
| **CMake** | 构建 | `C:\Users\xidon\program\CMake` |
| **Ninja** | 构建 | `C:\Users\xidon\program\Ninja\ninja.exe` |
| **FreeSWITCH** | SIP 服务器 / 回音测试 | `C:\Program Files\FreeSWITCH`（1.11.2） |

---

## 2. 宿主机手动设置（一次配置，重启不丢；需管理员）

### 2.1 ⚠️ 找到并重命名 TAP 网卡为 `tap0`（必须做，否则后续找不到 tap0）
OpenVPN 安装后，TAP 网卡（`InterfaceDescription` = `TAP-Windows Adapter V9`）的**连接名默认不是 `tap0`**，
通常是「本地连接 *1」「Ethernet 2」之类随机分配的名称。后续所有命令、QEMU `-nic tap,ifname=tap0`
以及本节配 IP 都依赖 `tap0` 这个名字，所以必须先重命名。

```powershell
# 1) 查出 TAP 网卡当前的连接名（看 Name 列）
Get-NetAdapter | Where-Object {$_.InterfaceDescription -match 'TAP'} |
    Format-Table Name,InterfaceDescription,Status,MacAddress -AutoSize
# 2) 重命名为 tap0（把 <查到的Name> 换成上一步实际看到的连接名）
Rename-NetAdapter -Name "<查到的Name>" -NewName "tap0"
# 3) 确认
Get-NetAdapter -Name "tap0"
```
> GUI 方式：`Win+R` → `ncpa.cpl` → 找到 `TAP-Windows Adapter V9` 那个网卡 → 右键「重命名」→ 输入 `tap0`。

改名后状态初始为 `Disconnected`（正常，QEMU 打开后变 `Up`）。

### 2.2 给 tap0 配静态 IP（管理员 PowerShell）
```powershell
Enable-NetAdapter -Name "tap0"
netsh interface ipv4 set address name="tap0" static 172.16.23.1 255.255.255.0
Get-NetIPAddress -InterfaceAlias "tap0" | Format-Table IPAddress,PrefixLength
# 预期输出: 172.16.23.1 / 24
```

### 2.3 ⚠️ 不要建网桥
- **绝对不要**尝试把 tap0 和以太网「桥接」——Hyper-V 下 Windows 网桥不可用，会直接报错。
- 不需要 NAT、不需要开 IP 转发。guest 和宿主是**终点直连**，路由天然可达。

### 2.4 可选：确认 Hyper-V 状态（用于排错）
```powershell
(Get-CimInstance Win32_ComputerSystem).HypervisorPresent   # True = Hyper-V 运行中
```

---

## 3. 固件代码修改（`boards/mps2-an505/FreeRTOS/application/`）

> 以下改动已落盘，新机器 clone 仓库后即为最终状态，无需再改。
> 若从旧版（slirp）代码升级，对照检查这 3 处。

### 3.1 `lwip_os_test.c` — guest 网络参数（`tcpip_init_done()`）
把 guest IP/网关/DNS/ping 目标设为 tap0 段：
```c
IP4_ADDR(&ipaddr, 172, 16, 23, 50);            // guest IP 172.16.23.50
IP4_ADDR(&netmask, 255, 255, 255, 0);
IP4_ADDR(&gw, 172, 16, 23, 1);                 // 网关 = 宿主 tap0
...
IP4_ADDR(&dns4, 172, 16, 23, 1);               // DNS（宿主 tap0；外网解析需宿主转发，电话用 IP 直连不依赖）
...
ip_2_ip4(&s_ping_target)->addr = lwip_htonl(LWIP_MAKEU32(172, 16, 23, 1));  // ping 目标 = 宿主 tap0
```

### 3.2 `pj_phone.c` — 电话应用（关键，注意区分两个 IP 的职责）
```c
#define HOST_GW   "172.16.23.1"     // 宿主 tap0
#define FS_HOST   "172.16.23.1"     // FS 实际监听地址（internal-lo 绑定它）→ 用于 reg_uri / proxy
#define PJ_PHONE_DIAL_HOST "192.168.23.7"  // ⚠️ 必须 = FreeSWITCH 的 domain（见下）
```
- **`PJ_PHONE_DIAL_HOST` 必须等于 FreeSWITCH 的默认 domain（`$${local_ip_v4}`）**，本机是 `192.168.23.7`。
  因为注册的 AOR 是 `sip:1000@<dial_host>`，FS 靠 AOR 的 domain 在 directory 里找用户；
  若改成 `172.16.23.1` 会报 `Can't find user 1000@172.16.23.1`（403）。
  **新机器上此值要改成新机器自己的 `local_ip_v4`。**
- **transport / 媒体：去掉所有 `public_addr = 127.0.0.1`**（slirp 遗留）。tap0 下 guest 用自己的真实 IP `172.16.23.50` 做 SDP `c=`，FS 直达。
  - `tcfg.public_addr`（SIP transport）→ 删掉
  - `acc_cfg.rtp_cfg.public_addr`（媒体）→ 删掉
- `reg_uri` / `proxy` 都指向 `sip:172.16.23.1:5060`（FS 实际地址）。

### 3.3 其他
- 其余（`REG_USER 1000`、`REG_PASSWORD 1234`、`GUEST_SIP_PORT 15062`）保持不变。

---

## 4. FreeSWITCH 配置修改（`C:\Program Files\FreeSWITCH\conf\`）

### 4.1 `sip_profiles/internal-lo.xml`
从 slirp 专用（`127.0.0.1` / `ext-rtp-ip=10.0.2.2` / `context=public`）改为 tap0：
```xml
<param name="context" value="default"/>         <!-- ⚠️ 必须 default，9196 echo 在 default dialplan -->
<param name="rtp-ip" value="172.16.23.1"/>
<param name="apply-nat-acl" value="deny.auto"/>
<param name="sip-ip" value="172.16.23.1"/>
<param name="ext-rtp-ip" value="172.16.23.1"/>
<param name="ext-sip-ip" value="172.16.23.1"/>
```
> 为什么 `context` 要 `default`：FS 的 9196 echo 应用配在 **default** dialplan
> （`conf/dialplan/default.xml` 的 `<extension name="echo"> destination_number ^9196$`）。
> 若保持 `public`，拨 9196 会 480 Temporarily Unavailable。

### 4.2 `autoload_configs/acl.conf.xml`
在 `domains` 列表放行 guest 网段（否则注册/来电被 ACL 拒）：
```xml
<list name="domains" default="deny">
  <node type="allow" domain="$${domain}"/>
  <!-- QEMU guest over tap0 segment (172.16.23.50). -->
  <node type="allow" cidr="172.16.23.0/24"/>
</list>
```

### 4.3 启动/重载 FreeSWITCH
- **手动运行**（服务可能显示 Stopped）：`C:\Program Files\FreeSWITCH\FreeSwitchConsole.exe`
- 改配置后（若 FS 已在跑）用 fs_cli 重载：
```powershell
& 'C:\Program Files\FreeSWITCH\fs_cli.exe' -H 127.0.0.1 -P 8021 -p ClueCon -x 'reloadxml'
& 'C:\Program Files\FreeSWITCH\fs_cli.exe' -H 127.0.0.1 -P 8021 -p ClueCon -x 'sofia profile internal-lo restart'
```
- ⚠️ **fs_cli 必须连 `127.0.0.1`**（event socket 的 ACL `loopback.auto` 只放行回环；连 `172.16.23.1` 会被拒）。
- 验证监听：`netstat -ano | findstr 5060` 应看到 `172.16.23.1:5060`。

---

## 5. 构建固件

```powershell
# 全新 configure（PJ_PHONE 必须 ON）
& 'C:\Users\xidon\program\CMake\bin\cmake.exe' -B build-phone -S . -G Ninja `
  -DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe `
  -DCMAKE_TOOLCHAIN_FILE=C:/Users/xidon/code/github/qemu-embedded-firmware/cmake/arm-none-eabi-gcc.cmake `
  -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DPJ_PHONE=ON

# 增量构建
& 'C:\Users\xidon\program\CMake\bin\cmake.exe' --build build-phone
# 产物: build-phone\boards\mps2-an505\FreeRTOS\an505-qemu.elf
```

---

## 6. 启动 QEMU（tap0）

```powershell
& 'C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe' `
  -machine mps2-an505 -cpu cortex-m33 -m 16M `
  -kernel 'C:\Users\xidon\code\github\qemu-embedded-firmware\build-phone\boards\mps2-an505\FreeRTOS\an505-qemu.elf' `
  -display none -serial file:C:\Users\xidon\code\github\qemu-embedded-firmware\works\tools\tap0-test-serial.log `
  -nic tap,ifname=tap0,script=no,downscript=no
```
- QEMU 打开 tap0 时它会从 `Disconnected` 变 `Up`（**非管理员即可打开 TAP 设备**）。
- 串口输出到文件便于读 guest 日志。如需抓包，把 `-nic tap,...` 改为
  `-nic tap,id=n0,ifname=tap0,script=no,downscript=no -object filter-dump,id=f0,netdev=n0,file=...pcap`。

---

## 7. 验证步骤（按序）

### 7.1 guest 网络起来
串口日志应有：
```
lwIP-OS up: IP 172.16.23.50 gw 172.16.23.1 MAC 52:54:00:12:34:56
```

### 7.2 UDP 命令服务器双向（端口 15000）
guest 固件内置 UDP 命令服务器（`phone_net.c`，命令：`dial/status/stat/hangup/host/rereg`）。
宿主发 `status` 应秒回（证明双向 UDP 通）：
```powershell
$u=New-Object Net.Sockets.UdpClient; $u.Client.ReceiveTimeout=3000
$b=[Text.Encoding]::ASCII.GetBytes('status'); $u.Send($b,$b.Length,'172.16.23.50',15000)
$e=New-Object Net.IPEndPoint([Net.IPAddress]::Any,0); $r=$u.Receive([ref]$e)
[Text.Encoding]::ASCII.GetString($r)   # 形如 reg=2 call=IDLE ... host=192.168.23.7
```

### 7.3 注册验证
```powershell
& 'C:\Program Files\FreeSWITCH\fs_cli.exe' -H 127.0.0.1 -P 8021 -p ClueCon -x 'sofia status profile internal-lo reg'
# 应看到:
#   User: 1000@192.168.23.7
#   Contact: "..." <sip:1000@172.16.23.50:15062;ob>
#   Status: Registered(UDP) ... Ping-Status: Reachable
```

### 7.4 通话测试（echo 9196）
```powershell
# 发 UDP 命令给 guest:15000
#   dial 9196   -> 拨号
#   status      -> 观察 call=ACTIVE
#   stat        -> 观察 rx/tx 增长、loss=0
```
预期：`call=ACTIVE peer=9196`，`stat` 的 `rx/tx` 持续增长（20 秒可达 rx=1400+），`loss=0`。
通话结束后用 `hangup` 挂断。

---

## 8. 常见坑与注意事项（务必看完）

1. **不要建网桥**：Hyper-V（`HypervisorPresent=True`）下 Windows Network Bridge 不可用，建桥必失败。
2. **DCO 无关**：`OpenVPN Data Channel Offload` 是 TUN 网卡，闲置不影响；用的是 `tap0`。
3. **`PJ_PHONE_DIAL_HOST` 必须是 FS 的 domain（`$${local_ip_v4}`）**，不是 tap0 IP。
   本机是 `192.168.23.7`；**换机器要改**。FS 日志报 `Can't find user 1000@<domain>` = 这个没配对。
4. **FS `internal-lo.xml` 的 `context` 必须是 `default`**，否则 9196 echo 拨不通（480）。
5. **fs_cli 连 `127.0.0.1:8021`**，连 `172.16.23.1` 会被 `loopback.auto` ACL 拒绝。
6. **ICMP 不可用（不影响业务）**：宿主 ping guest 超时 = guest 固件 lwIP 建了 ICMP raw PCB
   （ping 测试用）导致不回 echo；guest ping 宿主超时 = 宿主防火墙拦入站 ICMP。SIP/RTP 是 UDP，不受影响。
7. **120105 `No buffer space available`（已知遗留）**：第二次通话后再拨号会因 guest 固件
   lwIP socket 资源耗尽而失败。**单次通话完全正常**；多次测试前**重启 QEMU**。
   后续可排查 lwIP 的 `MEMP_NUM_UDP_PCB`/pbuf 池或 pjsua 通话结束的 socket 释放。
8. **tap0 IP 持久**：`netsh` 设的静态 IP 重启不丢。QEMU 每次启动会重新打开 tap0（变 Up）。
9. **FreeSWITCH 手动运行**：`FreeSwitchConsole.exe`；改配置后 `reloadxml` + `sofia profile internal-lo restart`。
10. **端口**：guest SIP `15062`、命令服务器 `15000`、FS SIP `5060`、event socket `8021`。RTP 动态端口。

---

## 9. 新机器部署 Checklist

- [ ] 安装 OpenVPN，**重命名 TAP 网卡为 `tap0`**（见 2.1，默认名不是 tap0）
- [ ] 确认 `tap0` 存在：`Get-NetAdapter -Name tap0`
- [ ] 安装 QEMU(arm)、ARM GCC、CMake、Ninja、FreeSWITCH
- [ ] 管理员：`Enable-NetAdapter tap0` + 配 `172.16.23.1/24`
- [ ] 确认 FS `internal-lo.xml`：`sip-ip/rtp-ip/ext-*=172.16.23.1`、`context=default`
- [ ] 确认 FS `acl.conf.xml`：domains 加 `172.16.23.0/24`
- [ ] 启动 FreeSwitchConsole，`netstat` 确认 `172.16.23.1:5060` 监听
- [ ] 固件代码：确认 `lwip_os_test.c` IP=172.16.23.50、`pj_phone.c` FS_HOST=172.16.23.1、
      **PJ_PHONE_DIAL_HOST=本机 local_ip_v4**、无 `public_addr=127.0.0.1`
- [ ] 构建（`cmake -B build-phone ... -DPJ_PHONE=ON` + `cmake --build build-phone`）
- [ ] QEMU 启动（`-nic tap,ifname=tap0,...`）
- [ ] 验证：串口 `lwIP-OS up: 172.16.23.50` → UDP `status` 有回复 → FS 注册表见 1000 → 拨 9196 双向媒体
