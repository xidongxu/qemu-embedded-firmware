# WORKLOG-2026-08-20 — 高精度时间戳（SysTick 组合计数器）

## 背景

用户怀疑"测试总丢帧不稳定"与系统时钟精度有关，提出把提了很久的 DWT 高精度时间戳做了。
pjlib port 的 `os_auto.h` 早已声明 `PJ_HAS_HIGH_RES_TIMER=1`（注释写明 "DWT cycle counter, see
os_timestamp_freertos.c"），但 Stage-1 用 `xTaskGetTickCount()` 占位（1 ms 分辨率），是遗留 TODO。

## 分析结论（先分析后动手）

**丢帧与时间戳精度的关系**：
- 丢帧**直接原因**（A/B 已证实）：slirp host 侧 UDP 随机丢包（`rx pkt<200`，MCU 根本没收到）+ RTP 突发到达（jbuf empty，PLC 填补）。**与 MCU 时钟精度无关**。
- **间接相关**：1 ms tick 造成 RTCP `rtt(avg)=0 / 4989us`（量化假象）、jbuf `delay dev=2-3ms`（量化噪声）——**统计不可信**，无法分辨"真抖动 2ms"还是"量化 2ms"。
- DWT 高精度时间戳的价值：① RTCP RTT/jitter、jbuf delay 统计真实化（亚毫秒）② 真实硬件必需（VoIP 抖动测量）③ 完成遗留 TODO。**不改变 slirp 丢包**（环境本质）。

## 实施过程

### 方案 1：DWT->CYCCNT + 运行时校准 freq —— ❌ 系统性回归
- 惰性使能 `DEMCR.TRCENA + DWT.CTRL.CYCCNTENA`；32 位 CYCCNT 64 位扩展（回绕 hi++）；freq 用 vTaskDelay(20ms) 窗口对 FreeRTOS tick 实测。
- **结果**：媒体崩溃（`rtcp tx pkt=44`、DTMF MISS、音频 weak、media FAILED）。
- **根因**：QEMU TCG 下 DWT CYCCNT 按**模拟 CPU 周期**计数，速率与墙钟（虚拟时钟）**不成固定比例**；运行时校准的 freq 只对校准窗口有效，随后漂移 → `pjmedia_clock` 的 10 ms interval 错误 → get/put 节奏乱。

### 方案 2：SysTick 组合计数器 —— ✅ 成功
- 实现：`pj_get_timestamp` = FreeRTOS tick（1 ms，高位）× `(LOAD+1)` + `(LOAD - SysTick->VAL)`（亚 ms 插值），freq = `SystemCoreClock`（25 MHz）。
- SysTick 与 FreeRTOS tick **同源**（QEMU 虚拟时钟），频率确定、无漂移 → clock interval 精确。
- 边界竞争：读 tick→VAL→若 tick 已变则重读。
- 依赖 `extern SystemCoreClock`（board `application/system_ARMCM33.c` 提供，25 MHz）。

## 验证

SysTick 版双 QEMU 通话回归：
```
RUN-A: 两边 media ALL PASSED | 音频 1001Hz loud 34 / 439Hz loud 38(满格)
RUN-B: 两边 media ALL PASSED | 音频 1001Hz loud 36 / 439Hz loud 38(满格)
RUN-C: callee rtt(avg)=4943us(真实) loss=0 empty=191 dtmf "5#" OK | media ALL PASSED
       caller rtcp loss=12(网络层丢 6%) empty=184 | media ALL PASSED
RUN-D(首轮): 两边 FAILED —— callee rtcp loss=75/200(37.5%，slirp 随机大丢)，
             但 rtt=3280us 真实、dtmf OK、音频 1001/439Hz 正常 → 网络层，非时钟
```

- **rtt 从量化的 0/4989us 变为真实 3280/4943us**——统计真实化达成。
- `rtcp tx pkt=200` 恢复正常（DWT 版是 44）——时钟正确。
- 丢帧仍为 slirp 网络随机（0-37.5% 波动），与时间戳无关；音频始终正常（PLC）。

## 关键教训

1. **QEMU TCG 下不要用 DWT CYCCNT 做时间基准**（速率与墙钟不一致 → 校准漂移）。
   **SysTick 与 tick 同源（虚拟时钟），可靠**。真硬件上 DWT 可再用（硬件周期=CPU 时钟）。
2. 高精度时间戳**改善测量**（RTCP/jbuf 统计可信），**不改变 slirp 丢包**。

## 涉及文件

- `libutils/pjprojec/ports/freertos/src/os_timestamp_freertos.c` — 改为 SysTick 组合计数器（CMSIS-free 寄存器访问 + extern SystemCoreClock）
