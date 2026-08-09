# 2026-08-08 工作记录：LAN9118 驱动工业级重构 + lwIP 对接（NO_SYS 与 FreeRTOS OS 模式）

> 本文档整理 2026-08-08 全天在 `qemu-embedded-firmware` 项目中完成的工作，**重点记录实现过程中遇到的所有"坑"与对应的解决办法**。
>
> 对应 git 提交：
> - `03a003b5` `feature(lan9118): 实现驱动以及对接lwip`
> - `d247c0d9` `feature(lwip): os支持`

---

## 目录

- [一、工作总览](#一工作总览)
- [二、功能实现清单](#二功能实现清单)
- [三、遇到的坑与解决方法（重点）](#三遇到的坑与解决方法重点)
  - [A. 驱动核心（LAN9118）](#a-驱动核心lan9118)
  - [B. lwIP NO_SYS 对接](#b-lwip-no_sys-对接)
  - [C. 外网连通性测试与代码整理](#c-外网连通性测试与代码整理)
  - [D. lwIP FreeRTOS OS 模式](#d-lwip-freertos-os-模式)
  - [E. 构建 / 运行环境](#e-构建--运行环境)
- [四、验证结果](#四验证结果)
- [五、构建与运行命令](#五构建与运行命令)
- [六、后续维护约定](#六后续维护约定)

---

## 一、工作总览

今天的工作分三个阶段完成，构成一条完整链路：**驱动 → 协议栈适配 → 外网验证 → 双模式（裸机/RTOS）运行**。

| 阶段 | 时间 | 内容 |
|------|------|------|
| 1 | 上午 | 将原有 LAN9118 驱动重构为**工业级分层架构**（协议栈无关），并接入 lwIP（NO_SYS 模式） |
| 2 | 上午~中午 | 新增**外网连通性测试**（DNS + TCP/HTTP），整理驱动代码格式，规范 `lan9118_regs.h` 头文件（宏前缀统一） |
| 3 | 中午~下午 | 为 lwIP 增加 **FreeRTOS OS 支持**（`NO_SYS=0` + `sys_arch`），采用"本地移植副本 + 上游保持纯净"策略，构建配置下沉到 `libutils` |

最终成果：`mps2-an505` 板卡在 QEMU 上，FreeRTOS 工程可**通过编译参数一键切换** lwIP 的 NO_SYS 模式与 OS（FreeRTOS 线程）模式，两种模式都能完成 ARP / ping / DNS / TCP / HTTP 外网通信。

---

## 二、功能实现清单

### 阶段 1：LAN9118 驱动工业级重构（`boards/mps2-an505/Core/`）

架构分为四层，驱动核心**零 RTOS / 协议栈依赖**：

```
lan9118_regs.h     全面寄存器定义（对齐 QEMU 模型 + 数据手册）
lan9118.h / .c     驱动核心：初始化、PHY、TX、RX、统计、回调
lan9118_osal.h/.c  OS 抽象层（FreeRTOS / 裸机双后端，编译宏切换）
lan9118_netif.h/.c lwIP 适配层（标准 ethernetif 模式，NO_SYS / RTOS 双支持）
```

核心能力：
- **初始化序列**：复位 → 探测（ID/ByteTest）→ MAC 地址（配置 → EEPROM → 回退）→ PHY 自动协商 → MAC/RX/AFC/中断配置
- **健壮 TX**：FIFO 空间管理、单包 MAC 的 `tx_busy` 完成跟踪、TX 状态 FIFO 错误统计（碰撞/载波丢失等）
- **中断驱动 RX**（兼保留裸机轮询拉模型：`read_frame` / `peek` / `rx_pending`）
- **RFC2863 风格统计** + 链路变化回调 + RX 投递回调
- 删除废弃兼容接口：`lan9118_receive` / `lan9118_xmit` / `lan9118_poll` / `lan9118_poll_loop` / `lan9118_test`

### 阶段 2：lwIP 对接与外网测试（`FreeRTOS/application/lwip_test.c`）

- 新增 `lwip_test.c`（仿照 `lv_disp.c` 结构）：
  - `lwip_task_init()` / `lwip_task_entry()` 专用 FreeRTOS 任务
  - NO_SYS 移植胶水：`sys_now()` / `sys_jiffies()` / `lwip_example_app_platform_assert()`
  - netif 初始化：静态 IP `10.0.2.15/24`，网关 `10.0.2.2`
  - NO_SYS 主循环：`lan9118_netif_poll()` + `sys_check_timeouts()`
- 测试通过两个独立开关选择：
  ```c
  #define LWIP_TEST_ICMP_PING        1U   /* ping 网关 10.0.2.2（保留） */
  #define LWIP_TEST_EXTERNAL_CONNECT 1U   /* DNS + TCP/HTTP 外网测试（新增） */
  ```
- 外网测试链路：配置 slirp DNS `10.0.2.3` → `dns_gethostbyname("www.baidu.com")` → `tcp_connect(:80)` → 发送 `GET / HTTP/1.1` → 打印响应；失败每 10s 自动重试。

### 阶段 3：lwIP FreeRTOS OS 模式（`lwip_os_test.c` + `libutils/lwip/ports/freertos/`）

- 新增编译参数 `-DLWIP_OS=none|FreeRTOS`（`PROJECT=FreeRTOS` 时默认 FreeRTOS）
- 新增 `lwip_os_test.c`：`tcpip_init` 线程 + **阻塞式 socket API**（`lwip_gethostbyname` / `socket` / `connect` / `send` / `recv`）
- FreeRTOS 移植基于 lwIP `contrib/ports/freertos`，**复制为本地副本** `libutils/lwip/ports/freertos/` 使用

---

## 三、遇到的坑与解决方法（重点）

### A. 驱动核心（LAN9118）

#### 坑 1：RX 状态字长度位读取错误（QEMU 下收不到帧）
- **现象**：驱动初始化正常、PHY link up，但 lwIP 一直收不到网卡上报的帧。
- **原因**：原驱动读 `sts & 0x7ff`（bit[10:0]）当作包长度，但 LAN9118 的状态字**长度实际在 bit[26:16]**，并且该长度**已包含 4 字节 FCS**。
- **解决**：改为
  ```c
  pkt_len = (sts >> 16) & 0x7ff;   /* 含 FCS */
  frame_len = pkt_len - 4;         /* 去掉 FCS */
  ```

#### 坑 2：TrustZone 陷阱 —— 调度器启动前调用 RTOS 原语导致 Lockup
- **现象**：`lan9118_open()` 在 `vTaskStartScheduler()` 之前执行，一旦内部调用 `vTaskDelay()` 或 `xSemaphoreCreateBinary()`，QEMU 直接报
  `Lockup: can't escalate 3 to HardFault`。
- **原因**：ARM_CM33_NTZ（TrustZone）下，调度器未启动时 RTOS 时钟/队列原语不可用。
- **解决**：OSAL 时间/延时做成**调度器感知**——未启动时改用 LAN9118 硬件 `FREE_RUN` 计数器计时（40ns tick，`ms = ticks / 25000`）；信号量改为**惰性创建**，首次 `take()`（必在任务上下文）时才创建。

#### 坑 3：CMSIS 设备头文件选错（FPU 编译错误）
- **现象**：`-mfpu=fpv5-sp-d16` 下编译报 FPU 相关错误。
- **原因**：默认包含的 `ARMCM33.h` 是 **no-FPU** 变体（`__FPU_PRESENT=0`）。
- **解决**：改用 **`ARMCM33_DSP_FP.h`**（FPU+DSP），与板级编译参数 `-DARMCM33_DSP_FP` 一致。

#### 坑 4：IRQ 48 中断向量缺失
- **现象**：网卡中断不触发。
- **原因**：GCC 启动文件 `startup_ARMCM33.s` 未登记 LAN9118 的中断向量（NVIC IRQ 48）。
- **解决**：在向量表补 `.long Interrupt48_Handler`，并让中断处理跳转到 `lan9118_isr`。

### B. lwIP NO_SYS 对接

#### 坑 5：`netif->output` 未设置 → `raw_sendto` 返回 `ERR_IF`
- **现象**：`raw_sendto()` 发送一直返回 `ERR_IF`，ping 发不出去。
- **原因**：`netif_add()` 只把 `output` 默认成 `netif_null_output_ip4`。
- **解决**：在 `lan9118_netif_init()` 里显式设置
  ```c
  netif->output      = etharp_output;
  netif->output_ip6  = ethip6_output;
  ```

#### 坑 6：`netif->linkoutput` 未设置 → 发 ARP 帧时崩溃
- **现象**：第一个 ARP 请求发出后任务直接卡死/崩溃。
- **原因**：`netif_add()` 不会初始化 `linkoutput`（默认 NULL），ARP 构造帧后调用 NULL 函数指针。
- **解决**：
  ```c
  netif->linkoutput = low_level_output;
  ```

#### 坑 7：lwIP 引入 libc malloc → 链接脚本符号缺失
- **现象**：链接报 `_sbrk` 相关未定义符号（`_end` / `_estack` / `_Min_Stack_Size`）。
- **原因**：lwIP 会拉入 libc 的 `malloc`，而 sysmem.c 的 `_sbrk` 需要 STM32 风格符号；MPS2 的 ld 脚本用的是 `__end__` / `__StackTop` / `__STACK_SIZE`。
- **解决**：在 `.heap` 段补充
  ```ld
  PROVIDE(_Min_Stack_Size = __STACK_SIZE);
  PROVIDE(_end = .);
  ```

#### 坑 8：FreeRTOS 堆不够 → `xTaskCreate` 返回 `-1`（rc=-1）
- **现象**：`lwip_task` 创建失败（`rc = -1`），网络任务没起来。
- **原因**：`configTOTAL_HEAP_SIZE` 只有 32KB，而 `main_task`(2048w) + `lv_task`(4096w) + `lwip_task`(4096w) 已经超过；注意 `xTaskCreate` 的栈参数单位是 **WORDS（4 字节）**，不是字节。
- **解决**：`configTOTAL_HEAP_SIZE` 提高到 **≥128KB**（`ucHeap` 跟随宏定义，位于 `application/main.c`）。

#### 坑 9：逐字符 `printf("%c")` 在 QEMU 下极慢
- **现象**：网络测试打印耗时异常，QEMU 交互卡顿。
- **原因**：轮询 UART 每字符一次输出，QEMU 下开销极大。
- **解决**：先缓冲到本地数组，最后 `printf("%s")` 一次性输出。

#### 坑 10：lwIP 应用必须提供的符号（缺一不可）
- NO_SYS 模式：`sys_now()`（= `xTaskGetTickCount`，tick=1000Hz）、`sys_jiffies()`、`lwip_example_app_platform_assert()`（`LWIP_PLATFORM_ASSERT` 需要）。
- **OS 模式注意**：这些由移植的 `sys_arch.c` 提供，**应用不能再定义**，否则重复定义冲突。

### C. 外网连通性测试与代码整理

#### 坑 11：slirp（`-nic user`）不转发外部 ICMP
- **现象**：ping `www.baidu.com` 无法通。
- **原因**：QEMU user 网络（slirp）只转发 TCP / UDP；ICMP echo **只对网关 `10.0.2.2` 模拟应答**，到外部主机的 ICMP 不转发（Windows 宿主尤其不支持）。
- **解决**：外网连通性测试不走 ICMP，改用 **DNS（UDP）+ TCP/HTTP**；保留 ping 测试但目标固定为网关 `10.0.2.2`。

#### 坑 12：必须使用本地修改版 QEMU
- **现象**：用系统 PATH 里的 QEMU，LCD 初始化会卡在 `lcd fb0=`，LVGL/LCD 全部起不来。
- **原因**：官方/系统 QEMU 对 `mps2-an505` 的 LCD 模型有问题。
- **解决**：一律使用本地补丁版
  ```
  C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe
  ```
  （打印 `mpsx simple lcd realize: 450x450` 即补丁版，LCD/TOUCH/LVGL 均正常）。

#### 坑 13：`lan9118_regs.h` 宏命名冲突风险（与 STM32 HAL 撞名）
- **现象**：`mps2-an505` 与 `stm32f405rg` 板卡宏名冲突隐患（如 `PHY_BCR` / `PHY_BSR` 在 `stm32f4xx_hal_conf.h` 里同名且值恰好相同，属于巧合，一旦同时编译就是宏重定义/值错误）。
- **决策过程**：先评估"寄存器级分组前缀"（`IRQ_CFG_IRQ_EN`、`MAC_CR_TXEN` 等）本身是工业界常见风格，不一定都要改；但**通用名（`PHY_*`、`INT_*`、`MAC_*`、`MII_*`）风险高**。最终用户决定：**全部统一加 `LAN9118_` 前缀**。
- **解决**：词边界批量改名 + 同步所有引用处（`lan9118.c` 等）+ 重新对齐宏值列。约 199 个宏全部统一为 `LAN9118_*` 前缀。

#### 坑 14：统一前缀时造成宏重定义（`-Wredefined`）
- **现象**：
  ```
  warning: 'LAN9118_PHY_ID1' redefined
    254: #define LAN9118_PHY_ID1  (0x02)
    291: #define LAN9118_PHY_ID1  (0x0007U)
  ```
- **原因**：批量改名脚本把两个不同含义的宏撞在一起——
  - `PHY_ID1/2`（PHY **寄存器地址** `0x02/0x03`）
  - `SMSC_LAN9118_PHY_ID1/2`（SMSC **厂商 ID 常量** `0x0007U/0xC0D1U`）
- **解决**：厂商 ID 常量保留 SMSC 语义、不与寄存器地址冲突：
  ```c
  #define LAN9118_PHY_ID1         (0x02)     /* PHY 寄存器地址 */
  #define LAN9118_PHY_ID2         (0x03)
  #define LAN9118_SMSC_PHY_ID1    (0x0007U)  /* SMSC 厂商 ID 常量 */
  #define LAN9118_SMSC_PHY_ID2    (0xC0D1U)
  ```

#### 坑 15：`Slirp: Failed to send packet, ret: -1` 报错
- **现象**：外网 HTTP 测试过程中 QEMU 打印 `Slirp: Failed to send packet, ret: -1`。
- **原因**：slirp 把 guest 的 TCP/UDP 包经宿主 socket 转发，对端（服务器）已关闭/重置连接后，guest 的 TCP 栈仍在发最后的 ACK 或重传段，写已关闭 socket 失败。Windows 宿主最常见。
- **结论**：**无害**。日志顺序已证明传输成功（先 `HTTP/1.1 200 OK`，后才出现该提示）。

#### 坑 16：头文件宏整理（未使用/重复宏的处理）
- **结论**：对全项目（`boards/` + `libutils/`，4309 个源文件）统计——199 个宏无重复；90 个"未使用"宏全部是 LAN9118 标准寄存器/位定义（对应手册与 QEMU 模型），属"便于后续扩展"，**全部保留**。仅做了**值列左对齐**（以最长宏名 `LAN9118_FIFO_INT_RX_STATUS_LEVEL(x)` 定列宽）。

### D. lwIP FreeRTOS OS 模式

#### 坑 17：errno 缺失（`ENOMEM` / `EINVAL` / `ENXIO` 未定义）
- **现象**：OS 模式链接报 errno 相关错误。
- **原因**：`NO_SYS=0` 才会编译 sockets / netdb / err.c，它们依赖 `ENOMEM` 等标准 errno 宏。
- **解决**：lwipopts 里 `#define LWIP_ERRNO_STDINCLUDE 1`，并在 `ports/arch/cc.h` 中 `#include <errno.h>`。

#### 坑 18：各 `*_MBOX_SIZE` 默认 0 → FreeRTOS 移植断言
- **现象**：运行时断言 `size > 0`。
- **原因**：lwIP 的 `TCPIP_MBOX_SIZE`、`DEFAULT_RAW/UDP/TCP_RECVMBOX_SIZE`、`DEFAULT_ACCEPTMBOX_SIZE` 默认都是 0。
- **解决**：在 lwipopts.h 全部设置非零值。

#### 坑 19：核心锁（最隐蔽）—— `sys_check_core_locking` 永远断言失败
- **现象**：OS 模式一运行就报 `Function called without core lock`，**即使从 `tcpip_thread` 内部调用也一样**。
- **原因**：开了 `LWIP_FREERTOS_CHECK_CORE_LOCKING=1` 后，默认的 `LOCK_TCPIP_CORE()` 映射到 `sys_mutex_lock(lock_tcpip_core)`，它**不会更新持有者/计数**，于是 `sys_check_core_locking()` 永远判定"未持有锁"。
- **解决**：把 `LOCK_TCPIP_CORE()` / `UNLOCK_TCPIP_CORE()` **重定向到移植自带的有跟踪版本**：
  ```c
  #define LOCK_TCPIP_CORE()   sys_lock_tcpip_core()
  #define UNLOCK_TCPIP_CORE() sys_unlock_tcpip_core()
  ```
  并在 lwipopts 里声明（FreeRTOS 移植的 `arch/sys_arch.h` 没有声明，unix/win32 移植才有）：
  ```c
  void sys_lock_tcpip_core(void);
  void sys_unlock_tcpip_core(void);
  void sys_mark_tcpip_thread(void);
  ```

#### 坑 20：链路回调上下文（netif 核心函数需要拿锁）
- **现象**：OS 模式下网卡链路变化回调里调用 `netif_set_link_up/down` 触发断言。
- **原因**：`netif_set_link_*` 是 **core 函数**，必须在 tcpip 核心锁内调用。
- **解决**：在 `lan9118_netif_link_cb` 中用 `LOCK_TCPIP_CORE()` / `UNLOCK_TCPIP_CORE()` 包裹（`#if !NO_SYS` 保护）。`lan9118_netif_thread` 作为任务运行（prio 3）。

#### 坑 21：`portTICK_RATE_MS` 过时（FreeRTOS V11 稳健性）
- **现象**：移植代码用 `portTICK_RATE_MS`，新版 FreeRTOS 提示改用 `portTICK_PERIOD_MS`。
- **解决**：本地副本 `ports/freertos/sys_arch.c` 中 `msleep` / `sem_wait` / `mbox_fetch` 三处改为 `portTICK_PERIOD_MS`（`sys_now` 原本就用 `portTICK_PERIOD_MS`）。

#### 坑 22：TCPIP 线程参数
- `TCPIP_THREAD_STACKSIZE` 设为 4096，**单位是 words**（因 `LWIP_FREERTOS_THREAD_STACKSIZE_IS_STACKWORDS=1`）；`TCPIP_THREAD_PRIO` 设为 4。

#### 坑 23：上游移植目录被改 → 升级 lwIP 会出问题
- **现象/担心**：直接修改 lwIP 自带的 `contrib/ports/freertos/`，后续升级 lwIP 时改动会丢失/冲突。
- **解决（本地副本策略）**：
  ```
  libutils/lwip/contrib/ports/freertos/   ← 上游，保持纯净（恢复官方原样）
  libutils/lwip/ports/freertos/           ← 本地定制副本（sys_arch.c + include/arch/sys_arch.h）
  ```
  构建系统全部指向本地副本；以后所有移植修改只改 `ports/freertos/`。

#### 坑 24：CMake 配置层级
- 顶层 `CMakeLists.txt` 不希望出现任何 lwIP 内容，lwIP 配置全部下沉到 `libutils/CMakeLists.txt`。
- **关键点**：`LWIP_OS` 用 **CACHE 变量**定义（全局可见），且加载顺序 `librtos → libutils → boards/...` 保证 `libutils` 先于板级运行，板级 `CMakeLists.txt` 才能正确读到 `LWIP_OS`。
- 首次配置按 `PROJECT=FreeRTOS` 自动默认 `FreeRTOS`，后续保留用户选择（`if(NOT DEFINED LWIP_OS)`）。

#### 坑 25：CMake 死代码 + 非法值兜底
- 优化点：
  1. 删掉恒真/恒假的内层 `if/else`（外层已包 `if(TARGET_PROJECT STREQUAL "FreeRTOS")`）；CACHE 变量不带 `FORCE` 只在未定义时写入，用户 `-DLWIP_OS=...` 依然保留。
  2. 非法值告警兜底：
     ```cmake
     if(NOT (LWIP_OS STREQUAL "FreeRTOS" OR LWIP_OS STREQUAL "none"))
         message(WARNING "Unsupported LWIP_OS=\"${LWIP_OS}\"; falling back to none (NO_SYS)")
         set(LWIP_OS none CACHE STRING "..." FORCE)
     endif()
     ```
  3. 消除重复字符串比较，导出局部布尔 `LWIP_OS_ENABLED`，便于未来扩展 `threadx` 等后端。

### E. 构建 / 运行环境

#### 坑 26：新增 `.c` 文件后"编译不到"
- **原因**：CMake 用 `file(GLOB)` 收集源文件，**glob 是配置期求值**。
- **解决**：新增源文件后必须重新执行 `cmake -S . -B build` 重新配置，再 `cmake --build build`。

#### 坑 27：外网 TCP 首次 SYN 常超时
- **现象**：`tcp_connect` 首次连接外网常失败/超时，重试后成功。
- **结论**：slirp NAT 对外的瞬态问题（记忆中也记录过"首次 SYN 常超时、每 10s 重试后成功"），代码里做自动重试即可。

#### 坑 28：QEMU 命令行参数
- 启动必须带上 `-nic user,model=lan9118` 并指定修改版 QEMU：
  ```bash
  qemu-system-arm.exe -machine mps2-an505 -cpu cortex-m33 -m 16M -nographic \
    -nic user,model=lan9118 -kernel <elf>
  ```
- 补丁版 QEMU 会打印 `mpsx simple lcd realize: 450x450`，LCD/TOUCH/LVGL 均正常（LVGL 9.5.0）。

---

## 四、验证结果

### 阶段 1（驱动 + NO_SYS 对接）
```
lwIP up: IP 10.0.2.15 gw 10.0.2.2 MAC 52:54:00:12:34:56
lwIP ping: reply from 10.0.2.2 seq=1 rtt_len=60 ...
lwIP: ping sent=4 recv=4 link=1     ← 100% 成功率
```
FreeRTOS / BareMetal / threadx 三个工程全部编译通过（`-Wall` 零警告）。

### 阶段 2（外网连通性测试，修改版 QEMU）
```
lwIP up: IP 10.0.2.15 gw 10.0.2.2
connect-test: DNS www.baidu.com -> 103.235.46.115        ✓ DNS
connect-test: TCP connected to 103.235.46.115:80          ✓ TCP
connect-test: RX[159]: HTTP/1.1 200 OK ... Content-Length: 29506 ... ✓ HTTP 200
connect-test: server closed, rx=30523 bytes (OK)
```

### 阶段 3（FreeRTOS OS 模式，修改版 QEMU）
```
lwIP-OS up: IP 10.0.2.15 gw 10.0.2.2 MAC 52:54:00:12:34:56
lwIP-OS http: DNS www.baidu.com -> 103.235.46.102        (netdb 阻塞解析)
lwIP-OS http: connected to www.baidu.com:80
lwIP-OS http: got 65731 bytes (OK)                        (socket TCP+HTTP)
lwIP-OS: ping sent=7 recv=7  http ok=1 fail=0 link=1      (raw API 跑在 tcpip 线程)
```
`-DLWIP_OS=none` 可正常回退 NO_SYS 模式（已验证两种模式都能编译链接）。

---

## 五、构建与运行命令

```bash
# 配置（PROJECT=FreeRTOS 默认启用 lwIP FreeRTOS OS 模式）
cmake -S . -B build -G Ninja \
  -DBOARD=mps2-an505 -DPROJECT=FreeRTOS \
  -DCMAKE_TOOLCHAIN_FILE="${PWD}/cmake/arm-none-eabi-gcc.cmake"

# 回到 NO_SYS（raw API）模式
cmake -S . -B build -DLWIP_OS=none

# 新增 .c 文件后务必重新配置
cmake -S . -B build

# 构建
cmake --build build
# 产物：build/boards/mps2-an505/FreeRTOS/an505-qemu.elf

# 运行（必须用本地修改版 QEMU）
C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe \
  -machine mps2-an505 -cpu cortex-m33 -m 16M -nographic \
  -nic user,model=lan9118 -kernel build/boards/mps2-an505/FreeRTOS/an505-qemu.elf
```

---

## 六、后续维护约定

1. **FreeRTOS 移植修改一律改** `libutils/lwip/ports/freertos/`（本地副本）；`libutils/lwip/contrib/ports/freertos/` 保持与 lwIP 官方一致，升级 lwIP 时直接替换 `libutils/lwip/` 即可。
2. **lwIP 相关构建配置**全部收拢在 `libutils/CMakeLists.txt`，顶层 `CMakeLists.txt` 不出现 lwIP 内容。
3. **`lan9118_regs.h`**：所有宏统一 `LAN9118_` 前缀、值列对齐；新增宏遵循同一风格，避免与 STM32 HAL 等库撞名。
4. 未来加第三种 OS 后端（如 threadx）只需在 `libutils/CMakeLists.txt` 的 `LWIP_OS` 分支加一处。
