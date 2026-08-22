# 实验 05 — DSP：回声消除 + 会议桥混音（pj_media_dsp_test.c）

**阶段**：pjmedia DSP 模块（stage 16）。

## 目的
验证 pjmedia 的两个 DSP 模块：
1. **AEC（声学回声消除）**：电话免提时，扬声器声音会经麦克风回灌形成回声，AEC 负责把"刚播出去的远端信号"从麦克风采集中减掉。
2. **会议桥（conference bridge）**：多方通话时把多路输入混成一路。

## 思路（纯合成信号，无真实音频）
- **AEC**：合成远端 1kHz（"正在播放"）与麦克风采集 = 0.5×1kHz 回声 + 0.5×2kHz 本地人声。`pjmedia_echo_cancel` 后，1kHz（回声）应被大幅抑制，2kHz（人声）应保留——用均方能量（定点，无 libm）度量。
- **会议桥**：两个音调发生器（697Hz + 1209Hz，即 DTMF "1" 的双音）加入 `pjmedia_conf` 并接到 master 端口；驱动 master `get_frame()` 应得到非零混音。

> 说明：AEC 后端选择 `echo_suppress`，因为 `PJMEDIA_HAS_SPEEX_AEC / WEBRTC_AEC / WEBRTC_AEC3` 均为 0（未编译那些第三方库）。

## 流程
1. AEC：`pjmedia_echo_create` → 喂远端参考帧 + 混入回声的采集帧 → `pjmedia_echo_cancel` → 比较消除前后的 1kHz/2kHz 能量比。
2. 会议桥：`pjmedia_conf_create` → 两个 `pjmedia_tonegen` 加入 → 连接 master → `pjmedia_conf_get_master_port` → `get_frame` 取混音。

## 学到什么
- **回声消除原理**：用"参考信号"（扬声器播的）估计并减去麦克风里的回声分量。
- pjmedia 的**媒体端口（port）模型**：任何音频源/处理器/混音器都是 `pjmedia_port`（有 `get_frame/put_frame`），可任意串接。
- 会议桥（`pjmedia_conf`）的多路混音与 master 端口机制。
- 定点 DSP（`PJ_HAS_FLOATING_POINT=0`）下的能量度量技巧。

## 如何启动
默认构建（`pj_media_dsp_test_run()` 在 04 之后执行）。

## 成功标准
串口打印 `pj_media_dsp: PASSED`（AEC 抑制回声 + 会议桥混音非零）。
