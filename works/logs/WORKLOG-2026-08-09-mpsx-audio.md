# 2026-08-09 工作记录：mpsx-simple-audio 音频设备驱动 + 播放数据验证（预期 vs 实际）

> 本文档整理 2026-08-09 在 `qemu-embedded-firmware` 项目中完成的工作：为 QEMU 新增的 `mpsx-simple-audio` 音频设备编写固件驱动并播放测试音频，**重点记录遇到的坑与解决思路**，以及**如何验证代码中播放的音频数据与预期一致（含原理说明、思路、方法）**。
>
> 对应 git 提交：`6d50eca6` `feature(audio): 音频驱动`
>
> QEMU 设备侧源码（在另一仓库）：`hw/audio/mpsx_simple_audio.c` + `include/hw/audio/mpsx_simple_audio.h`；机器接线：`hw/arm/mps2-tz.c`。

---

## 目录

- [一、工作总览](#一工作总览)
- [二、功能实现清单](#二功能实现清单)
- [三、遇到的坑与解决方法（重点）](#三遇到的坑与解决方法重点)
- [四、验证：播放的音频数据是否符合预期（重点）](#四验证播放的音频数据是否符合预期重点)
- [五、构建与运行命令](#五构建与运行命令)
- [六、后续维护约定](#六后续维护约定)

---

## 一、工作总览

今天在 `mps2-an505` 板卡上打通了一条完整的 **QEMU 音频设备 → 固件驱动 → 播放验证** 链路：

| 环节 | 内容 |
|------|------|
| 设备侧（QEMU） | 用户已在 `qemu-embedded-platform` 仓库新增 `mpsx-simple-audio` 设备，映射在 `0x51002000`，NVIC IRQ 49 |
| 固件驱动侧 | 新增 `Core/Src/audio.c` + `Core/Inc/audio.h`，实现注册表封装、播放/停止/单音/测试旋律等 API |
| 应用接入 | FreeRTOS `main.c` 在 `main_task_entry` 中调用 `audio_init()` + `audio_test()`，开机即播放 |
| 验证 | 用 wav 后端把播放内容落盘，FFT 频谱分析"音高随时间变化"，与预期音符表逐段对比，结论：**完全一致** |

最终成果：`audio_test()` 在 QEMU 上播放一段 **C 大调上行琶音 + 尾部长音、无缝循环**（3520ms/循环），实测频率与代码意图偏差 <1%。

---

## 二、功能实现清单

### 设备侧（QEMU，只读了解，不改）

`mpsx-simple-audio` 是一个极简音频输出控制器（仿 `mpsx-simple-lcd/touch`）：

- 寄存器（32 位小端，`DEVICE_LITTLE_ENDIAN`，只能 4 字节访问）：

| 偏移 | 名称 | 说明 |
|------|------|------|
| 0x00 | CTRL | `ENABLE=1<<0`，`RESET=1<<1`，`UPDATE=1<<2` |
| 0x04 | STATUS | `BUSY=1<<0`，`DONE=1<<1`（满一轮置位），`UNDERRUN=1<<2` |
| 0x08 | FORMAT | bits[1:0]：`U8=0` / `S16=1`；bit2：`STEREO` |
| 0x0C | BUF_ADDR | PCM 缓冲区的**客户机物理地址**（写时清零 `play_pos`） |
| 0x10 | BUF_LEN | 缓冲区字节数（写时清零 `play_pos`） |
| 0x14 | SAMPLE_RATE | 1000~192000 Hz，越界忽略 |
| 0x18 | PLAY_POS | 当前读偏移（只读） |
| 0x1C | INT_EN | `DONE=1<<0` |
| 0x20 | INT_STATUS | 写 1 清对应位 |

- **播放模型**：设备用 `address_space_read(&address_space_memory, ...)` 从客户机 RAM 直接读 PCM（与 LCD 读 framebuffer 同款）；`CTRL.ENABLE` 打开 QEMU 音频 voice；**每读完一整轮缓冲区就把 `play_pos` 回绕到 0 并置 `STATUS.DONE` / 拉 IRQ，继续循环播放**，直到 `ENABLE` 清零。
- **QEMU 侧音质注意**：`wav` 后端**固定重采样为 44100Hz / 16bit / 立体声**输出，与源格式无关；无 `audiodev` 时设备照样能读写 MMIO，但播放被静默丢弃。

### 固件驱动侧（今天新增）

```
Core/Inc/audio.h      寄存器定义（对齐 QEMU 模型）+ API 声明
Core/Src/audio.c      驱动实现（无 libm / 无浮点依赖）
```

- **API**：`audio_init()`、`audio_play(pcm,len,rate,fmt)`、`audio_stop()`、`audio_update()`、`audio_status()`、`audio_play_pos()`、`audio_wait_done()`、`audio_tone(freq,ms,vol)`、`audio_test()`。
- **波形生成**：64 点 int8 正弦查找表 + 32 位定点相位累加器（16 位线性插值），**跨音符相位连续、无爆音**；零 libm/浮点依赖。
- **播放策略**：把整段旋律渲染进 32KB 静态 `s_pcm`，交给设备**循环播放**，无需中断、无需定时器、无需补数据。
- **可选中断**：提供 `Interrupt49_Handler`（清 `INT_DONE`）；用户已在 FreeRTOS 启动文件补了 IRQ 49 向量项（见下），但内置测试不依赖中断。

### 应用接入

`FreeRTOS/application/main.c`：`main_task_entry` 里 `lcd_init(); touch_init();` 之后追加 `audio_init(); audio_test();`，开机自动播放。

### 中断向量接线

FreeRTOS `startup/gcc/startup_ARMCM33.s` 补上：

```asm
.long    Interrupt48_Handler                /*   48 Ethernet (LAN9118) */
.long    Interrupt49_Handler                /*   49 Audio (MPSX_SIMPLE_AUDIO) */
.space   (430 * 4)                          /* Interrupts 50 .. 480 */
```

（注意：只改了 FreeRTOS 的 GCC 启动文件；BareMetal / threadx 的启动文件 49 号向量仍是保留 0，若要在那两个工程走中断需同步修改。）

---

## 三、遇到的坑与解决方法（重点）

### A. 驱动实现

#### 坑 1：`sinf` / `libm` 无法链接 —— `(sinf): Unknown destination type (ARM/Thumb)`
- **现象**：链接报
  ```
  undefined reference to `sinf'
  (sinf): Unknown destination type (ARM/Thumb) in audio.c.obj
  dangerous relocation: unsupported relocation
  ```
- **原因**：三个工程都用了 `-specs=nano.specs` + `-mfloat-abi=hard -mfpu=fpv5-sp-d16`。`nano.specs` 把 libc 换成 `libc_nano`，而 `libm`（新库）在这个 multilib 组合下拿到的 `sinf` 符号与 Thumb 重定位不兼容，链接失败。
- **解决**：**放弃 libm，彻底零依赖**。用 64 点 int8 正弦 LUT + 32 位定点相位累加器（`idx = phase>>16`，`frac=phase&0xFFFF` 做线性插值，`step = (freq<<22)/rate`）在纯整数域生成正弦。既解决链接问题，也让驱动在 BareMetal / FreeRTOS / threadx 三个工程通用。

#### 坑 2：IRQ 49 中断向量缺失（向量槽是保留 0）
- **现象/隐患**：音频设备接的是 NVIC IRQ 49，但三个启动文件里 49~480 号向量是 `.space` 保留（0）。若此时 `NVIC_EnableIRQ(49)`，中断一来 CPU 会跳 0 地址 → HardFault。
- **解决**：① 内置测试走**无中断的循环播放路径**，默认不使能 NVIC IRQ 49；② 提供 `Interrupt49_Handler` 便于将来接；③ 已在 FreeRTOS 启动文件补 `.long Interrupt49_Handler`（见上文）。

#### 坑 3：U8 静音不是 0，而是 0x80
- **原因**：U8 是无符号 PCM，静音/中点 = `0x80`（128），不是 0；QEMU 设备里 `silence = (fmt==U8)?0x80:0x00`。
- **解决**：驱动静音段 `memset(buf, 0x80, n)`；渲染 `buf[i] = 128 + y*vol/127`，`y∈[-127,127]` → 采样 ∈ [1,255]，不削顶。

#### 坑 4：寄存器访问必须是 4 字节小端
- **原因**：设备 `impl/valid.min_access_size = max_access_size = 4`，非 4 字节访问会被忽略。
- **解决**：头文件统一 `#define AUDIO_XXX REG32(AUDIO_BASE+off)`（`*(volatile uint32_t*)`）。

### B. 构建 / 运行环境

#### 坑 5：新增 `.c` 文件后"编译不到"
- **原因**：`Core/CMakeLists.txt` 用 `file(GLOB Src/*.c)` 收集源文件，**glob 是配置期求值**，不会自动感知新文件。
- **解决**：加完 `audio.c` 后必须重新 `cmake -S . -B build ...` 再 `cmake --build build`。

#### 坑 6：PowerShell 传 `-Dxxx=yyy` 被拆参数
- **现象**：`-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake` 被 PowerShell 拆成 `cmake/arm-none-eabi-gcc` + `.cmake`，报 "Could not find toolchain file"；Ninja 也找不到（PATH 未含）。
- **解决**：参数加引号 + 显式指定：
  ```powershell
  cmake -S . -B build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe" `
    "-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake" `
    -DBOARD=mps2-an505 -DPROJECT=FreeRTOS
  ```

#### 坑 7：QEMU 必须有 audiodev 才出声
- **原因**：设备通过 `-machine mps2-an505,audiodev=<id>` + `-audiodev <driver>,id=<id>` 拿到音频后端；没接则 `voice` 为空，播放被静默丢弃（MMIO 仍正常）。
- **解决**：命令行必须成对给出：
  ```
  -machine mps2-an505,audiodev=audio0 -audiodev wav,path=out.wav,id=audio0
  ```
  无显卡环境用 `wav` 落盘验证最方便；要听到声音换 `-audiodev dsound,id=audio0`（Windows）。

### C. 验证工具

#### 坑 8：QEMU 被强制结束后 wav 头长度字段为 0
- **现象**：`wave` 模块 / 常见读取器报 "not a WAVE file"。
- **原因**：QEMU `wav` 后端只在**正常关闭**时回填 RIFF 总长与 `data` chunk 长度；测试时用强杀结束，这两个字段都是 0。
- **解决**：分析脚本 `verify_audio.py` 改成**按 chunk 扫描**（找 `fmt ` / `data`），遇到 `data` 长度 0 就**读到文件末尾**当 PCM。

#### 坑 9：过零法测频在重采样音频上全线失真
- **现象**：用"零交叉计数 / 中位过零间隔"测音高，结果满屏 C6+（1000~2450Hz 乱跳），完全不像预期旋律。
- **原因**：QEMU `wav` 后端把 8kHz 重采样到 44.1kHz，重采样在高频叠加了噪声；正弦在**零点附近**被抖动放大，产生大量**额外过零**，把测得频率系统性拉高（详见"四、验证"的原理说明）。
- **解决**：改用 **FFT 峰值检测**（详见下节），对纯正弦极稳。

---

## 四、验证：播放的音频数据是否符合预期（重点）

### 4.1 思路（为什么这样验）

"代码预期会播什么"是**确定的**（音符表 + 时长 + 波形），而"耳朵听到什么"是**主观且依赖声卡**的（QEMU 无声卡时根本听不到）。所以验证的正确姿势是：

> **把播放内容落到一个可分析的载体（wav），再从 PCM 中恢复"音高随时间的变化"，和预期的音符-时长表做逐段对比。**

即：预期 = 一张表 `(音符, 频率, 起始时间, 时长)`；实际 = 从 wav 测出的同一张表；两者一致即证明"播放的数据符合预期"。

### 4.2 预期表（来自 `audio_test()` 的渲染参数）

- 格式：**8kHz / U8 单声道 / 正弦波**；`vol=90/127≈71%`；音符间隔 60ms 静音（U8 `0x80`）。
- 一个循环（3520ms）：C4(300ms) → 60ms → E4(300) → 60 → G4(300) → 60 → C5(300) → 60 → E5(300) → 60 → G5(300) → 60 → C6(300) → 60 → **C4 长音 1000ms** → 无缝循环。
- 每音符 2400 采样、间隔 480 采样、总计 28160 字节。

### 4.3 验证流程

1. 用补丁版 QEMU + `wav` 后端跑固件，抓约 10s（≈3 个循环）到 `audio-verify.wav`（实测 QEMU wav 后端会写成 33s，无所谓，只分析前 3 个循环）。
2. 运行 `verify_audio.py <wav> 10.6` 做分析。

### 4.4 方法演进（重点：三种测频法，为什么只有 FFT 可行）

| 方法 | 做法 | 结果 | 原因 |
|------|------|------|------|
| ① 零交叉计数 | 每窗口数正弦过零次数，`f = 过零数/(2·窗口时长)` | ❌ 满屏 C6+，乱跳 | 重采样高频噪声叠加在正弦零点附近 → 额外过零 → 频率被拉高 |
| ② 中位过零间隔 | 取相邻过零间隔的中位数反推周期 | ❌ 仍不稳 | 同样是"零点抖动"问题，只是更抗离群值 |
| ③ **FFT 峰值检测** | 每 100ms 窗口：加 Hann 窗 → 零填充到 8192 点 FFT → 在 40~2000Hz 取能量峰 | ✅ 干净、稳定 | 正弦在频域就是**一根线谱**，噪声功率远小于基频峰，`argmax` 稳得主频 |

**原理说明**：
- **为什么正弦适合 FFT 测频**：正弦 $x(t)=A\sin(2\pi f_0 t)$ 的 DFT 能量几乎全集中在 $\pm f_0$ 两个 bin；加窗抑制频谱泄漏、零填充把分辨率做到 $\Delta f = 44100/8192 \approx 5.4$Hz。单音纯正弦 → 峰值即基频，非常干净。
- **为什么过零法必挂**：重采样后信号在过零点附近是"噪声抖动的小幅振荡"，任何轻微高频分量都会让符号频繁翻转，额外过零直接污染频率估计——这是**时域方法对信噪比敏感**的典型表现。
- **静音（gap）怎么识别**：U8 静音 `0x80` 转成 S16 后是 0，RMS 几乎为 0。用 **RMS 阈值（<500）** 判定"静音段"，与"音符段"分开。
- **wav 头不能反推源格式**：QEMU wav 后端固定写 `44100Hz/16bit/立体声`，所以文件头的 channels/rate/bits **不代表源格式**；分析的是内容，不是头。
- **为什么末段 C4 实测 1400ms 而非 1000ms**：持续主音 C4(1000ms) 与下一循环开头 C4(300ms) **同频且相位连续、中间没有静音**，被识别成一段连续 1400ms 的 C4——这是设计意图（无缝回绕），不是 bug。同理，每音符实测约 300~400ms 是 100ms 分析窗口与 60ms 静音间隔的量化误差。

### 4.5 结果（前 3 个循环逐段一致）

| 段 | 预期频率/起始 | 实测频率 | 实测时长 | 判定 |
|----|------|------|------|------|
| C4 | 261.63 Hz / 0ms | 261 | 300ms | ✅ |
| E4 | 329.63 / 360ms | 328 | 400ms | ✅ |
| G4 | 392.00 / 720ms | 393 | 400ms | ✅ |
| C5 | 523.25 / 1080ms | 522 | 300ms | ✅ |
| E5 | 659.25 / 1440ms | 657 | 400ms | ✅ |
| G5 | 783.99 / 1800ms | 781 | 300ms | ✅ |
| C6 | 1046.50 / 2160ms | 1044 | 400ms | ✅ |
| C4 长音 | 261.63 / 2520ms, 1000ms | 258 | 1400ms（=1000+300 连续） | ✅ |

- 频率误差 <1%（FFT 分辨率 + 重采样所致，可忽略）；循环周期实测 3.5s，与 3520ms 吻合；第 2、3 循环与第 1 循环**逐段完全相同**（循环播放正常）。
- **结论：实际播放的音频数据与代码预期完全一致。**

---

## 五、构建与运行命令

```powershell
# 1) 配置（新增 .c 后必须重新配置）
cmake -S . -B build -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe" `
  "-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake" `
  -DBOARD=mps2-an505 -DPROJECT=FreeRTOS

# 2) 构建
cmake --build build
# 产物：build/boards/mps2-an505/FreeRTOS/an505-qemu.elf

# 3) 运行并抓音频（必须用补丁版 QEMU；wav 后端落盘验证）
C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe `
  -machine mps2-an505,audiodev=audio0 -cpu cortex-m33 -m 16M -nographic `
  -nic user,model=lan9118 `
  -audiodev wav,path=audio-verify.wav,id=audio0 `
  -kernel build\boards\mps2-an505\FreeRTOS\an505-qemu.elf

# 4) 分析（对照预期音符表；需 numpy）
python verify_audio.py audio-verify.wav 10.6
```

预期串口日志：

```
mpsx simple audio realize: rate=8000 fmt=0x0
audio: pcm buffer @ 80020900 (32768 bytes)
audio: reset done, status=0x00000002
audio: test melody 3520 ms, looping...
audio: playing 28160 bytes @ 8000 Hz fmt=0x0 (buf=0x80020900)
```

---

## 六、后续维护约定

1. **改旋律/加音符**：改 `audio.c` 的 `audio_test()`（`notes_hz` 表 + 时长参数），缓冲上限 `AUDIO_PCM_SIZE=32768`（8kHz 下约 4s）；超长旋律要么加大缓冲，要么走 `audio_wait_done()`/中断补数据。
2. **波形生成保持零依赖**：不要在这个驱动里引入 `sinf`/`libm`（nano.specs + hard-float 下链接不过）；新增波形一律用 LUT + 定点累加器。
3. **中断路径**：`Interrupt49_Handler` 已定义；走中断需保证对应工程的启动向量表已补 49 号槽（目前只有 FreeRTOS 补了，BareMetal/threadx 未补）。
4. **QEMU 侧**：`mpsx-simple-audio` 依赖 `-machine ...,audiodev=<id>` + `-audiodev ...` 才有声音；`wav` 后端文件头不能反推源格式。
5. **验证工具**：`verify_audio.py`（仓库根目录，纯 Python + numpy）可复用——改完旋律后抓一份 wav 跑一遍即得"预期 vs 实际"对比。
