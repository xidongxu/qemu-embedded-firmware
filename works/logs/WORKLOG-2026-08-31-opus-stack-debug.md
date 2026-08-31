# WORKLOG 2026-08-31 — Opus 48k "跑不动" 排查复盘（根因 = 任务栈溢出）

> 目的：完整记录从"Opus 48k 呼叫卡死"到"根因定位并修复"的**全过程**——现象、错误假设、
> 验证方法、调试工具链、根因、修复、可复用方法论。供以后遇到"系统卡死 / 无响应 /
> QEMU 下媒体异常"类问题时直接参考。
>
> 相关：roadmap 2.1（宽带语音）。代码修复点：`mpsx_dev.c`（任务栈）。

---

## 1. 现象（最初观察）

在 pj_phone（pjsua）里启用 Opus 48k 全频后：

- SDP 协商正常：`audio updated, stream #0: opus (sendrecv)`，FS 通道 `read=opus rate=48000`
- 但随后 **系统完全卡死**：
  - 串口无任何输出（连每 3s 打印一次的 watchdog 都不打）
  - 呼叫卡在 `CONNECTING`（无 `CONFIRMED`、无 ACK）
  - `call_out.wav` 全 0（媒体静音）
  - QEMU 进程**占满 1 个 CPU 核**（guest 在忙等/死循环）
- 对比：**G.722 16k 完全正常**（双向 RTP 零丢包）。G.711/PCMU 也正常。

---

## 2. 排查过程（含错误假设的推翻）

### 2.1 第一轮假设（错误）：Opus 48k 编码 CPU 不够

直觉：M33 25MHz + TCG 解释执行，48k Opus 编码太重在实时预算（20ms/帧）内跑不完 →
媒体线程饿死其他任务。

**验证方法：写一个最小独立基准 `opus_bench.c`**（`PJ_PHONE_OPUS_BENCH=1`，专用
`build-bench` 目录，纯 CPU、无 pjsua/网络/lwIP，main 只跑这一个任务）。

**结论：假设被推翻。** 实测：
```
rate=48000 cplx=0 enc=1088 us/f (5.4% of 20ms) dec=799 us/f
rate=48000 cplx=5 enc=2065 us/f (10.3% of 20ms) dec=896 us/f
```
编码最坏只占 **10% 的 20ms 预算** —— CPU 完全够。**不是性能问题。**

> ⚠️ 教训：bench 直接调 `opus_encode` 调用链浅，**不会复现** pjsua 的深层调用链问题，
> 因此 bench 不崩是"假阴性"，会误导判断。但它证明了 CPU 能力，缩小了范围。

### 2.2 观察层：判断"忙等"还是"等中断"

QEMU 占满核 = guest 在忙等/死循环（不是休眠等中断）。验证方法：
```powershell
# 看 QEMU 进程 CPU 消耗（2s 内消耗 ~2000ms = 占满 1 核）
$c1=$p.CPU; Start-Sleep 2; $p.Refresh(); $c2=$p.CPU; ($c2-$c1)*1000
# 同时看 call_out.wav 是否仍增长（QEMU 侧写 wav 不依赖 guest 串口）
```

**结论**：guest 在忙等 → 指向自旋/死循环/异常死循环，不是信号量等待。

### 2.3 用 QEMU HMP monitor 抓 CPU 现场（关键工具）

重启 QEMU 带 monitor：`-monitor tcp:127.0.0.1:4444,server,nowait`
然后连上 `stop` + `info registers` 抓 PC：
```powershell
# works/tools/hmp_stop.ps1
```
抓到：`R15=0x10005c0a`，`XPSR=... handler` —— **CPU 在 `HardFault_Handler` 里死循环**！

读 fault 寄存器（`fault_dump.py`，HMP `xp`）：
- `HFSR=0x40000000`（FORCED 强制 HardFault）
- `CFSR=0`（无具体标志 —— QEMU 的 M33 fault 状态模拟不完整，不可全信）

**结论**：不是死锁，是 **HardFault**（异常死循环）→ 系统所有任务停 + 占满核。

### 2.4 接入 fault-dump，转储 HardFault 现场

默认 `HardFault_Handler: b .`（死循环无转储）。工程自带 `fault-dump` 模块
（`fault_dump_handler(unsigned int *stack, unsigned int linker)` 会打印异常帧寄存器 +
调用栈 + 当前任务）。

**接入**（startup_ARMCM33.s）：
```asm
HardFault_Handler:
    movs    r0, #4
    mov     r1, lr
    tst     r0, r1          ; EXC_RETURN bit2: 0=MSP 1=PSP
    beq     .use_msp
    mrs     r0, psp
    b       .call
.use_msp:
    mrs     r0, msp
.call:
    mov     r1, lr
    ldr     r2, =fault_dump_handler
    blx     r2
    b       .
```
> 注：`fault_dump_init()` 已在 main.c 调用。此转储**保留**（对任何 future crash 都有价值）。

**转储解读**（注意 fault-dump 读取偏移：它把异常帧当作 r4-r11 之后，实际异常帧在
point[0..7]，需把 R4-R11 当成 R0-R3,R12,LR,PC,PSR 读）：
```
CurrentTask: mpsx_play
(异常帧: 实际 PC = 0x100F9E70, LR = 0x100F9DCD)
Stack Call: 100F9DC8 100F6A48 100F24DE 100F1784 100F3D72 100F4208 100E0972 ...
```

### 2.5 addr2line 定位调用链（关键）

```powershell
arm-none-eabi-addr2line -f -e an505-qemu.elf 0x100F9E70
```
```
0x100F9E70  silk_resampler_private_down_FIR
0x100F9DCD  silk_resampler
0x100F6A48  silk_Encode
0x100F24DE  opus_encode_frame_native
0x100F3D72  opus_encode_native
0x100F4208  opus_encode
0x100E0972  codec_encode (opus.c, pjmedia)
0x100D8134  put_frame (stream.c, pjsua)
```
**HardFault 在 Opus 编码器内部的 SILK 降采样器**，调用链来自 pjsua 媒体发送路径。

### 2.6 反汇编 fault PC，看崩溃指令（关键）

```powershell
arm-none-eabi-objdump -d an505-qemu.elf | Select-String '^100f9e'
```
```
100f9e50: e92d 4ff0   stmdb sp!, {r4-r9,sl,fp,lr}
100f9e54: b093       sub sp, #76
100f9e64: eb08 0204   add.w r2, r8, r4
100f9e68: 0092       lsls r2, r2, #2
100f9e6c: 3207       adds r2, #7
100f9e6e: f022 0207  bic.w r2, r2, #7
100f9e70: ebad 0d02   sub.w sp, sp, r2     ; ← HardFault HERE（动态 alloca，SP 越界）
```
**fault 指令是 `sub.w sp, sp, r2`** —— SILK 重采样器用**动态栈分配（alloca）**，SP 下移
越界 → HardFault。r2 由 resampler 状态字段算出（fault 时 r2=0x810=2064，正常值）→
**不是状态被破坏，是当前 SP 已经太低（栈几乎用满），再减 2064 就越界。**

### 2.7 确认栈溢出（逐级加大验证）

- 任务栈 `MPSX_TASK_STACK=2048` 字（**8KB**）→ HardFault（崩在 silk_resampler alloca）
- 加大到 `4096` 字（16KB）→ **仍崩**（同一位置，说明 16KB 也不够）
- 加大到 `8192` 字（**32KB**）→ ✅ **成功**：`state=5 CONFIRMED`、双向 RTP 零丢包、
  conf sig 有信号、wav 主频 1001Hz（1kHz 回声清晰）

用 `uxTaskGetStackHighWaterMark` 佐证：32KB 栈 task 启动时 hwm=8172（充足），但
**pjsua 深度调用链**（`play_cb → conf bridge → stream → codec_encode → opus_encode →
silk` 动态 alloca）叠加后需求 >16KB。

---

## 3. 根因

**mpsx_play / mpsx_cap 任务栈太小（8KB），Opus 48k 编码在 pjsua 深度调用链 + SILK
动态 alloca 下栈需求超过 16KB → SP 越界 → HardFault → 系统卡死。**

为什么 G.722/PCMU 不崩：这些编码器调用链浅、无大动态 alloca。

## 4. 解决方案

```c
// mpsx_dev.c
#define MPSX_TASK_STACK  8192   /* 原 2048 字(8KB) → 32KB */
```
play/cap 两个任务共用此宏。32KB × 2 = 64KB，heap 256KB 足够。

## 5. 验证（3 轮回归）

| 轮 | CONFIRMED | rx/tx | rx_lost | wav 主频 |
|---|---|---|---|---|
| 1 | ✅ | 2086/2093 | 0 | 1001Hz |
| 2 | ✅ | 2092/2101 | 2 (0.1%) | 1001Hz |
| 3 | ✅ | 2073/2080 | 0 | 1001Hz |

Opus 48k 全频在 QEMU/TCG 稳定可用。

---

## 6. 可复用方法论 / 工具清单

**症状 → 判断流程**：
1. 系统卡死 + QEMU 占满核 → **busy-wait / HardFault 死循环**（不是等中断）
2. HMP `info registers` 抓 PC → 若在 `HardFault_Handler` → **HardFault**
3. fault-dump 转储（startup 接入）→ 异常帧 PC/调用栈/当前任务
4. `addr2line` 定位调用链 → `objdump` 反汇编 fault PC 看具体指令
5. 若 fault 在 `sub sp,sp,rN`（alloca）→ **栈溢出** → 逐级加大任务栈验证

**工具文件**（works/tools/）：
- `opus_bench.c` — 独立 Opus 基准（`PJ_PHONE_OPUS_BENCH` 宏 + 独立 build-bench）
- `hmp_stop.ps1` — HMP 连 QEMU，stop + info registers 抓 PC
- `fault_dump.py` — HMP xp 读 CFSR/HFSR/BFAR/MMFAR
- `gdb_opus.gdb` — GDB stub 异步暂停（HMP 更稳，GDB 批处理 `continue &`+interrupt 不可靠）

**关键命令速查**：
```powershell
# 启动带 HMP monitor
qemu-system-arm.exe ... -monitor tcp:127.0.0.1:4444,server,nowait
# 定位
arm-none-eabi-addr2line -f -e build-phone/boards/mps2-an505/FreeRTOS/an505-qemu.elf 0x<PC>
arm-none-eabi-objdump -d build-phone/boards/mps2-an505/FreeRTOS/an505-qemu.elf | Select-String '^<addr>:'
```

**经验教训**：
1. **独立 bench 是"假阴性"温床**：调用链浅复现不了深层栈问题；但它能排除 CPU 性能
   嫌疑，缩小范围，仍值得做。
2. **QEMU 占满核 = guest 忙等/异常死循环**，是"卡死"与"等中断"的关键判别。
3. **HMP monitor + fault-dump + addr2line + objdump** 是嵌入式无调试器时的
   四板斧，比盲猜高效得多。
4. 栈溢出症状（HardFault + 深层编码路径 + 动态 alloca）特征明显，应优先怀疑
   **任务栈**，尤其新增重 codec（Opus/视频）后。
5. M33 的 QEMU fault 状态寄存器（CFSR）可能为 0，不可全信，要以 PC/调用栈为准。
