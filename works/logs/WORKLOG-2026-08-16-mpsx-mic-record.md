# 2026-08-16 工作记录：mpsx-simple-mic 录音设备驱动 + 测试应用 + 中断方式验证

> 本文档整理在 `qemu-embedded-firmware` 项目中完成的工作：为 QEMU 新增的 `mpsx-simple-mic` 麦克风设备编写固件驱动与 FreeRTOS 测试应用，打通"WAV 录音源 → QEMU 设备 → guest 内存 → 驱动采集 → 信号分析"全链路，并**将采集方式从轮询升级为 NVIC 中断驱动并验证中断真正触发**。**重点记录遇到的坑与解决办法**，以及**驱动后续的使用方法**。
>
> 对应 git 提交：本次变更**尚未提交**（固件仓库最近提交为 `2d73aac2` 2026-08-15 chore(baresip)）。涉及文件：
> - 新增 `Core/Inc/mic.h`、`Core/Src/mic.c`
> - 新增 `FreeRTOS/application/mic_test.h`、`mic_test.c`
> - 修改 `FreeRTOS/application/main.c`（接入 `mic_init()/mic_test()`）
> - 修改 `FreeRTOS/startup/gcc/startup_ARMCM33.s`（安装 `Interrupt50_Handler` 向量项，用户改动）
>
> QEMU 设备侧源码（在另一仓库 `qemu-embedded-platform`）：`hw/audio/mpsx_simple_mic.c` + `include/hw/audio/mpsx_simple_mic.h`；机器接线：`hw/arm/mps2-tz.c`（`0x51003000`，`get_sse_irq_in(mms, 50)`）。

---

## 目录

- [一、工作总览](#一工作总览)
- [二、功能实现清单](#二功能实现清单)
- [三、遇到的坑与解决方法（重点）](#三遇到的坑与解决方法重点)
- [四、验证结果](#四验证结果)
- [五、构建与运行命令](#五构建与运行命令)
- [六、驱动后续使用方法](#六驱动后续使用方法)

---

## 一、工作总览

在 `mps2-an505` 板卡上打通了一条完整的 **QEMU 麦克风设备 → 固件驱动 → 录音验证 → 中断驱动** 链路：

| 环节 | 内容 |
|------|------|
| 设备侧（QEMU，前置） | `qemu-embedded-platform` 新增 `mpsx-simple-mic` 设备，映射 `0x51003000`，NVIC IRQ 50；新增 `infile`（WAV 测试源）属性与 4 个采样率的测试音频 |
| 固件驱动侧 | 新增 `Core/Src/mic.c` + `Core/Inc/mic.h`：寄存器封装 + `mic_init/mic_capture/mic_stop/...`，采集方式**先轮询后升级为中断** |
| 测试应用 | 新增 `FreeRTOS/application/mic_test.c/h`：采集 1 秒 S16，分析峰值/平均幅度/过零率，判定 PASS/FAIL |
| 应用接入 | FreeRTOS `main.c` 在 `main_task_entry` 中调用 `mic_init() + mic_test()` |
| 中断接线 | 用户在 `startup_ARMCM33.s` 向量表补 `Interrupt50_Handler`（slot 50），驱动使能设备/NVIC 中断后，采集由中断标志驱动 |
| 验证 | 用 `audio_test_8k.wav` 作录音源，串口输出 `irq_done=1` + `mic_test: signal detected -> PASSED`，`peak=26214` 与源 WAV 的 1kHz 正弦幅值（0.8×32767）**完全吻合** |

最终成果：mic 驱动支持**中断方式**采集（保留轮询兜底），固件在 QEMU 上开机即录音验证并 PASS。

---

## 二、功能实现清单

### 设备侧（QEMU，只读了解，不改）

`mpsx-simple-mic` 是极简音频输入（录音）控制器，与 `mpsx-simple-audio`（播放）寄存器镜像：

- 寄存器（32 位小端，仅 4 字节访问）：

| 偏移 | 名称 | 说明 |
|------|------|------|
| 0x00 | CTRL | `ENABLE=1<<0`，`RESET=1<<1`，`UPDATE=1<<2` |
| 0x04 | STATUS | `BUSY=1<<0`，`DONE=1<<1`（写满一轮置位），`OVERRUN=1<<2` |
| 0x08 | FORMAT | bits[1:0]：`U8=0` / `S16=1`；bit2：`STEREO` |
| 0x0C | BUF_ADDR | 采集缓冲的**客户机物理地址**（写时清零 `rec_pos`） |
| 0x10 | BUF_LEN | 缓冲区字节数（写时清零 `rec_pos`） |
| 0x14 | SAMPLE_RATE | 1000~192000 Hz，越界忽略 |
| 0x18 | REC_POS | 当前写偏移（只读） |
| 0x1C | INT_EN | `DONE=1<<0`，`OVERRUN=1<<1` |
| 0x20 | INT_STATUS | 写 1 清对应位 |

- **录音模型**：设备把采集到的 PCM 用 `address_space_write(&address_space_memory, ...)` 直接写入 guest RAM（与 audio 播放的 `read` 方向相反）；**每写满一整轮就把 `rec_pos` 回绕到 0、置 `STATUS.DONE`，若 `INT_EN.DONE` 使能则拉 IRQ 50**，然后继续循环写入，直到 `ENABLE` 清零。
- **infile 模式（WAV 测试源）**：`-global mpsx-simple-mic.infile=xx.wav` 后，设备用 `QEMUTimer` 按 **WAV 自身采样率**循环读文件喂给 guest；不转换格式（文件是什么字节就写什么字节），因此 **guest 的 `SAMPLE_RATE` 必须与 WAV 一致**，`FORMAT` 也应按 WAV 位深配（16bit 文件配 `S16`）。
- **真实麦克风**：不配 infile，用 `-audiodev ...in.voices=1` + `-global mps2-an505.audiodev=<id>`。

### 固件驱动侧（今天新增）

```
Core/Inc/mic.h      寄存器定义（对齐 QEMU 模型）+ API 声明
Core/Src/mic.c      驱动实现（轮询 + 中断双路径，无 libm/浮点依赖）
```

- **API**：`mic_init()`、`mic_capture(buf,len,rate,fmt,timeout)`、`mic_stop()`、`mic_update()`、`mic_status()`、`mic_rec_pos()`。
- **采集流程**（`mic_capture`）：`mic_stop()` → 写 `FORMAT/SAMPLE_RATE/BUF_ADDR/BUF_LEN` → 清 `INT_STATUS` → 记录中断计数基准 → `CTRL=ENABLE|UPDATE` 启动 → **等中断标志变化（主）或轮询 `STATUS.DONE`（兜底）** → 返回后缓冲内即为 `len` 字节采集数据。
- **中断路径**：`mic_init` 中 `MIC_INT_EN=MIC_INT_DONE` + `NVIC_EnableIRQ(50)`；`Interrupt50_Handler` 写 1 清 `INT_STATUS.DONE` 并递增 `volatile s_mic_irq_done`；`mic_capture` 通过该标志判定一轮完成，并在打印中带 `irq_done` 计数（用于证明中断真的触发）。

### 测试应用（今天新增）

`FreeRTOS/application/mic_test.c/h`：`mic_test()` 调用 `mic_capture()` 采 1 秒（8000 字节 = 4000 个 16bit 样本），计算 **峰值 / 平均绝对值 / 过零率**（全整数运算），峰值与均值超过阈值即判定"有信号 → PASSED"，否则提示检查录音源与采样率。

### 应用接入

`FreeRTOS/application/main.c`：`main_task_entry` 里 `audio_init(); audio_test();` 之后追加 `mic_init(); mic_test();`，开机即录音验证。

### 中断向量接线（用户改动）

`FreeRTOS/startup/gcc/startup_ARMCM33.s` 向量表：

```asm
.long    Interrupt49_Handler                /*   49 Audio (MPSX_SIMPLE_AUDIO) */
.long    Interrupt50_Handler                /*   50 Mic (MPSX_SIMPLE_MIC) */
.space   (429 * 4)                          /* Interrupts 51 .. 480 */
```

（原为 `.space (430*4)` 保留 50..480；末尾已有 `Set_Default_Handler Interrupt50_Handler` 弱定义，`mic.c` 的强定义会覆盖。）

---

## 三、遇到的坑与解决方法（重点）

### 坑 1：infile 模式采样率必须与 WAV 一致
- **现象**：`SAMPLE_RATE` 配错（如 WAV 是 8000 却配 44100）时，采集内容按 WAV 速率喂入，guest 按自己的速率解释，结果**变速/缓冲错位**。
- **解决**：`mic_capture` 的 `rate` 与所选 WAV 采样率一致（8k/16k/44k/48k 文件对应 8000/16000/44100/48000）。infile 模式下设备**不转换格式**，`FORMAT` 也应按 WAV 位深（16bit→`S16`）配置。

### 坑 2：固件无 libm/浮点，信号统计不能用 sqrt/sinf
- **现象**：newlib-nano + hard-float 下链接 `sinf/sqrt` 容易失败（audio 驱动同样踩过）。
- **解决**：测试统计只用整数运算——峰值（max|v|）、平均绝对幅度（sum|v|/n）、过零率（符号翻转次数），阈值判定信号是否存在。不依赖浮点库。

### 坑 3：printf 不能在中断里用
- **现象**：若在 `Interrupt50_Handler` 里直接 `printf`，可能与串口/其他中断重入冲突。
- **解决**：handler 只做"写 1 清中断 + 递增 `volatile` 标志"，全部打印放主流程（`mic_capture`/`mic_test`）。

---

## 四、验证结果

### 轮询版（初版，WAV 录音源）

```
===== mic test (mpsx-simple-mic) =====
mic: captured 8000 bytes @ 8000 Hz fmt=0x1 (buf=0x80020b28, rec_pos=192)
mic_test: captured 4000 samples (peak=26214 mean_abs=379 zcr=24)
mic_test: signal detected -> PASSED
```

### 中断版（最终，向量表 + 驱动中断路径）

```
mic: reset done, status=0x00000000 (IRQ 50 enabled)          ← NVIC IRQ 50 已使能
mic: captured 8000 bytes @ 8000 Hz fmt=0x1 (buf=0x80020b28, rec_pos=192, irq_done=1)
mic_test: captured 4000 samples (peak=26214 mean_abs=379 zcr=24)
mic_test: signal detected -> PASSED
```

- `peak=26214` = `0.8 × 32767`，与 `audio_test_8k.wav` 中 1kHz 正弦的峰值**完全一致** → WAV → QEMU 设备 → guest 内存 → 驱动采集 → 整数分析全链路正确。
- `irq_done=1` → 采集是通过 `Interrupt50_Handler` 置位的标志完成的，**中断真正触发**。
- 固件其余功能（LCD / Touch / Audio 播放 / FatFS / lwIP）不受影响，均正常。

---

## 五、构建与运行命令

### 固件构建（CMake + Ninja）

```powershell
# 固件仓库根目录
cmake -B build -S . -G Ninja `
  -DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe `
  -DCMAKE_TOOLCHAIN_FILE=C:/Users/xidon/code/github/qemu-embedded-firmware/cmake/arm-none-eabi-gcc.cmake `
  -DBOARD=mps2-an505 -DPROJECT=FreeRTOS

cmake --build build
```

产物：`build/boards/mps2-an505/FreeRTOS/an505-qemu.elf`

### QEMU 运行（录音源 = WAV 文件）

```powershell
qemu-system-arm.exe -machine mps2-an505 -cpu cortex-m33 -m 16M `
  -kernel <build>\an505-qemu.elf `
  -global mpsx-simple-mic.infile=<qemu-embedded-platform>\testcase\audio_test_8k.wav `
  -display none -serial stdio
```

### QEMU 运行（录音源 = 真实麦克风）

```powershell
... -audiodev dsound,id=aud0,in.voices=1 -global mps2-an505.audiodev=aud0 ...
```

### 测试音频文件（`qemu-embedded-platform/testcase/`）

| 文件 | 采样率 | 对应 SAMPLE_RATE |
|------|--------|------------------|
| `audio_test_8k.wav` | 8000 | 8000 |
| `audio_test_16k.wav` | 16000 | 16000 |
| `audio_test_44k.wav` | 44100 | 44100 |
| `audio_test_48k.wav` | 48000 | 48000 |

每段 5 秒 16bit 单声道，内容：静音 → 1kHz → 440Hz → 对数扫频 → 白噪声。

---

## 六、驱动后续使用方法

### 基本采集（一轮）

```c
#include "mic.h"

static int16_t s_cap[4096];          /* 8000B = 4000 samples */

mic_init();                          /* 复位 + 使能 IRQ 50 + 清中断 */

if (mic_capture((uint8_t *)s_cap, sizeof(s_cap), 8000,
                MIC_FORMAT_S16, 10000000UL)) {
    /* s_cap 内为 8000 字节（4000 个 16bit）PCM，可直接处理 */
}
mic_stop();
```

- `rate` 必须与录音源一致（WAV 用其采样率；麦克风用 `-audiodev in.frequency`）。
- `fmt`：16bit 源用 `MIC_FORMAT_S16`，8bit 源用 `MIC_FORMAT_U8`。

### 连续多轮采集（中断计数差值，无需轮询）

设备写满一轮就触发一次 DONE 中断，`Interrupt50_Handler` 累加 `s_mic_irq_done`。可记录"上一轮"计数，等待差值变化即表示新的一轮数据就绪：

```c
/* 建议在 mic.h 暴露查询函数：uint32_t mic_irq_count(void); */
uint32_t base = mic_irq_count();
mic_update();                              /* 重新从缓冲头开始 */
while (mic_irq_count() == base) { /* 等下一轮 */ }
/* 缓冲区已被新数据覆盖 */
```

### 说明与约定

- 新增 `.c` 文件放 `Core/Src/`（驱动）或 `FreeRTOS/application/`（应用），CMake `file(GLOB)` 自动收集，**无需改构建文件**；头文件放 `Core/Inc/`。
- 中断 handler 只操作 MMIO + volatile 标志，**不要调用 FreeRTOS API / printf**。
- 若换板子且向量表未装 `Interrupt50_Handler`，`mic_capture` 自动退化为轮询 `STATUS.DONE`，仍可工作（但 `irq_done` 恒为 0）。
- 本次 mic 相关变更**尚未 git 提交**，建议及时提交：
  - `Core/Inc/mic.h`、`Core/Src/mic.c`
  - `FreeRTOS/application/mic_test.h`、`mic_test.c`
  - `FreeRTOS/application/main.c`、`FreeRTOS/startup/gcc/startup_ARMCM33.s`
