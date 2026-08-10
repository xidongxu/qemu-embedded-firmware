# 2026-08-10 工作记录：mps2-an505 外部 SPI NOR flash（w25q02jvm）+ 固件工业级驱动

> 本文档整理 2026-08-10 在 `qemu-embedded-firmware` 项目中完成的工作：为用户自定义 QEMU 的 `mps2-an505` 机器挂载外部 SPI NOR flash（Winbond `w25q02jvm`，256 MiB），并编写一套面向文件系统的工业级固件驱动。**重点记录整体实现思路、遇到的坑与解决、以及如何验证实现是否正确**。
>
> 涉及两个仓库：
> - QEMU 侧（`qemu-embedded-platform/qemu`，改动未提交，由用户自行 review/提交）：`hw/arm/mps2-tz.c`、`hw/misc/mps2-fpgaio.c/.h`
> - 固件侧（`qemu-embedded-firmware`）：新增 `boards/mps2-an505/Core/Src/spi_flash.c` + `Core/Inc/spi_flash.h`；`FreeRTOS/application/main.c` 接入

---

## 目录

- [一、工作总览](#一工作总览)
- [二、功能实现清单](#二功能实现清单)
- [三、整体实现思路](#三整体实现思路)
- [四、遇到的坑与解决方法（重点）](#四遇到的坑与解决方法重点)
- [五、验证：如何确认实现是正确的（重点）](#五验证如何确认实现是正确的重点)
- [六、构建与运行命令](#六构建与运行命令)
- [七、后续维护约定](#七后续维护约定)

---

## 一、工作总览

今天打通了一条完整的 **QEMU 外部 SPI flash → 固件工业级驱动 → 实测验证** 链路：

| 环节 | 内容 |
|------|------|
| 分析 | 确认"仅靠 `-drive` 启动参数挂不了 flash"，必须改 QEMU 机器源码 |
| QEMU 侧 | `w25q02jvm` 挂到 PL022 `spi0`（0x40205000）的 SSI 总线；**CS 直接由 FPGAIO MISC bit8 控制**（不新增独立 CS 设备，贴合真实 AN505 硬件） |
| 固件驱动侧 | 新增 `Core/Src/spi_flash.c` + `Core/Inc/spi_flash.h`：错误码、参数校验、忙轮询+超时、WREN、4 字节地址命令、跨页自动拆分、事务级临界区、可选 OS 锁 |
| 应用接入 | FreeRTOS `main.c` 在 `main_task_entry` 中 `spi_flash_init()` 并打印器件信息 |
| 验证 | JEDEC `EF 70 22` 识别正确；`selftest` 用 4 字节地址命令完成 擦→写→读回；`m25p80_select` trace 观察到 CS 边沿，证明 FPGAIO→CS 连接生效 |

最终成果：QEMU 中 mps2-an505 拥有一颗 256 MiB SPI NOR flash，固件驱动零警告编译通过，可正常擦写读，为后续对接文件系统（如 LittleFS）做好了底层准备。

---

## 二、功能实现清单

### 1. QEMU 侧（改动未提交，用户自行 review/提交）

| 文件 | 改动 |
|------|------|
| `include/hw/misc/mps2-fpgaio.h` | `MPS2FPGAIO` 增加 `qemu_irq spi_cs;`（GPIO 输出） |
| `hw/misc/mps2-fpgaio.c` | `mps2_fpgaio_init` 注册 named GPIO out `"spi-cs"`；`A_MISC` 写处理：`s->misc=value; qemu_set_irq(s->spi_cs, extract32(value,8,1))`（bit8=0 选中 / 1 释放）；reset 复位 CS=0；补 `#include "hw/core/irq.h"` |
| `hw/arm/mps2-tz.c` | `make_spi()` 对 `fpga_type==FPGA_AN505 && name=="spi0"` 创建 `w25q02jvm` 挂到 PL022 的 `"ssi"` 总线、绑定 `-drive if=mtd`；`mps2tz_common_init()` 末尾连接 FPGAIO `"spi-cs"` → flash `SSI_GPIO_CS` |

- CS 方案要点：真实 AN505 的 SPI CS 线就是 FPGAIO MISC 寄存器控制的，**直接复用 MISC bit8**，固件写 `FPGAIO_MISC` 即可精确控制 flash 片选（有 select/deselect 边沿），比"CS 常选"更贴近硬件、更可靠。

### 2. 固件驱动侧（今天新增）

```
boards/mps2-an505/Core/Inc/spi_flash.h      头文件：器件参数、错误码、API、LittleFS glue 示例
boards/mps2-an505/Core/Src/spi_flash.c      驱动实现
```

- **API**：`spi_flash_init(cfg)` / `deinit` / `get_info` / `read` / `write` / `erase_sector(4K)` / `erase_block_32k` / `erase_block(64K)` / `erase_chip` / `sync` / `selftest`。
- **错误码**：`OK / ERR_PARAM / ERR_IO / ERR_TIMEOUT / ERR_PROBE / ERR_NOT_INIT / ERR_WRITE`。
- **4 字节地址命令**：`READ4 0x13`、`PP4 0x12`、`ERASE4_4K 0x21`、`ERASE4_32K 0x5c`、`ERASE4_64K 0xdc`（>16 MiB 必须），不碰芯片非易失"进入 4 字节模式"配置。
- **线程安全**：每个事务在短临界区（`__get_PRIMASK`/`__disable_irq` 保存恢复）内完成 CS-assert→传输→CS-release，防 ISR 撕裂；提供 weak 的 `spi_flash_os_lock/unlock` 供 FreeRTOS mutex 覆盖实现多任务互斥。
- **文件系统友好**：头文件内附 LittleFS `read/prog/erase` 三函数适配示例。

### 3. 应用接入

`FreeRTOS/application/main.c`：`main_task_entry` 中 `audio_test()` 之后：

```c
rc = spi_flash_init(NULL);
if (rc == SPI_FLASH_OK) {
    spi_flash_get_info(&fi);
    printf("spi_flash: JEDEC %02X %02X %02X, size=%uMiB, page=%u, sector=%u, 4B-addr=%d\r\n", ...);
} else {
    printf("spi_flash: init failed (%d)\r\n", (int)rc);
}
```

---

## 三、整体实现思路

### 1. 先回答"要不要改源码"——分析机器现状

- 查 `mps2-tz.c` 的 `make_spi()`：上游**只创建 PL022 控制器**，其内部 `"ssi"` 总线上没有挂任何 SPI 从设备。
- 结论：`-drive if=mtd` 只是提供一个 BlockBackend（后端存储），必须由机器代码把 `m25p80` 实例化、接到 `"ssi"` 总线、并把它的 `drive` 属性指向该后端，flash 才会出现在总线上。**只靠启动参数做不到**（`-global` 只能覆盖已存在设备的属性，不能凭空挂设备）。

### 2. CS 控制方案选型

- 真实 AN505 的 SPI CS 由 FPGAIO MISC 寄存器控制 → 直接改 `mps2-fpgaio.c`，让 MISC bit8 输出一个 `"spi-cs"` GPIO。
- `m25p80` 从设备自带 `"ssi-gpio-cs"` 输入；固件写 MISC bit8（0 选中 / 1 释放）→ CS 边沿 → 状态机回 IDLE。
- 这样**不需要**独立的 `mpsx_spi_flash_cs` 设备，少一个设备、更贴近硬件、固件驱动也更简单。

### 3. 固件驱动分层

```
API 层   read/write/erase_sector/...（参数校验 + OS 锁 + 跨页拆分）
命令层   jedec/read4/page_program4/erase4/chip_erase（WREN + 事务 + 忙轮询）
底层     PL022 8bit master 传输 + FPGAIO MISC CS + 临界区 + DWT 超时
```

- 面向文件系统：字节寻址、任意长度读、自动跨页写、多种擦除粒度、`get_info` 提供几何参数。
- 全部校验：地址越界、长度越界、擦除对齐、空指针、未初始化。

### 4. 验证策略（先建"证据链"）

详见第五节。核心是：**设备挂没挂上**（设备树）→ **CS 通没通**（trace）→ **能不能读写**（固件实测）→ **代码干不干净**（零警告 + git diff）。

---

## 四、遇到的坑与解决方法（重点）

### A. QEMU 侧

#### 坑 1：头文件路径随版本变化 —— `hw/irq.h` 不存在
- **现象**：`mps2-fpgaio.c` 编译 `fatal error: hw/irq.h: No such file or directory`。
- **原因**：QEMU v11.1.0-rc3 把中断相关头挪到了 `include/hw/core/irq.h`（旧版是 `include/hw/irq.h`）。
- **解决**：改用 `#include "hw/core/irq.h"`（`qemu_set_irq`、`qemu_irq`、`qdev_init_gpio_out_named` 声明所在）。

#### 坑 2：只给 `-drive` 挂不上 flash（误解点）
- **现象**：用户以为可能只要 `-drive if=mtd` 就能用。
- **原因**：`-drive` 只创建 BlockBackend；必须有机器代码把 `m25p80` 实例化并接总线。
- **解决**：改源码在 `make_spi()` 里 `qdev_new("w25q02jvm")` → `qdev_realize_and_unref` 到 `"ssi"` 总线，并用 `qdev_prop_set_drive(flash, "drive", blk_by_legacy_dinfo(dinfo))` 绑定后端。

#### 坑 3：SSIBus 是不透明类型，拿不到 `->bus` 成员
- **现象**：想写 `qdev_realize_and_unref(flash, &ssi->bus, ...)`，但 `struct SSIBus` 定义在 `ssi.c` 内部，头文件里不可见。
- **解决**：用 `BusState *ssi = qdev_get_child_bus(DEVICE(spi), "ssi")`，直接传 `BusState*` 给 `qdev_realize_and_unref`（第二个参数就是 `BusState*`）。参考 `imx25_pdk.c` 挂 SD 卡的写法。

#### 坑 4：`info block` 显示 flash 挂在 `/machine/unattached/device[0]`（一度误判为"没挂上"）
- **现象**：`info block` 显示 `mtd0 Attached to: /machine/unattached/device[0]`，`info qom-tree /machine/spi0/ssi` 又看不到子设备，一度以为挂载失败。
- **原因**：QEMU 里 **QOM 对象树（parent/child）与 qbus 总线-设备关系是分离的**。`qdev_new` 创建的对象 QOM parent 默认在 `unattached` 容器；`qdev_realize(dev, bus)` 只把设备加入 bus 的 children 链表，**不改变 QOM parent**。所以 `info block` 里的路径是 QOM 路径（`unattached`），而 `info qtree` 里 flash 确实在 pl022 的 `bus: ssi` 下。
- **解决/教训**：**判断设备是否在总线上以 `info qtree` 为准**（看 `bus: ssi ... dev: w25q02jvm`），不要被 `info block` 的 QOM 路径误导。

#### 坑 5：`m25p80` 状态机只在 CS 释放（deassert）时回 IDLE
- **原因**：`m25p80_cs(ss, select)` 里 `select==true`（高电平=释放）才把状态复位到 `STATE_IDLE` 并提交数据。若 CS 一直选中，写完 PP 后状态机停在 `STATE_PAGE_PROGRAM`，后续命令全乱。
- **解决**：固件驱动**每笔事务都必须 toggle CS**（先 MISC bit8=0 选中 → 传输 → bit8=1 释放）。这也是本方案用 FPGAIO 精确控 CS 的意义所在。

### B. 固件构建 / 编译

#### 坑 6：`hw/irq.h` 路径问题在固件侧没有，但 CMSIS 依赖要注意（略，见坑 1）

#### 坑 7：PowerShell 下 CMake 参数被拆断 + Ninja 找不到
- **现象**：`cmake -DCMAKE_TOOLCHAIN_FILE=cmake\arm-none-eabi-gcc.cmake ...` 报 `Could not find toolchain file: cmake\arm-none-eabi-gcc`，且 `Ninja` generator 找不到 `ninja`。
- **原因**：PowerShell 把 `cmake\arm-none-eabi-gcc.cmake` 里的 `.cmake` 截断；Ninja 生成器需要显式 `CMAKE_MAKE_PROGRAM`。
- **解决**：工具链路径用引号包裹并改用正斜杠；追加 `-DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe`。

#### 坑 8：函数定义顺序导致隐式声明（-Werror）
- **现象**：`spi_flash_wait_busy()` 里调用了定义在其后的 `spi_flash_xfer()`/`spi_flash_cs()`，报 implicit declaration。
- **解决**：在这两个函数前加 static 前向声明。

#### 坑 9：未使用函数触发 `-Wunused-function`
- **现象**：写了一个 `spi_flash_now_ms()` 没用上，`-Wall -Werror` 下报错。
- **解决**：删除未使用函数。

#### 坑 10：`-Wmaybe-uninitialized` —— 结构体未初始化
- **现象**：`spi_flash_info_t info;` 传给 `get_info` 后打印，编译器保守报 may be used uninitialized。
- **解决**：`spi_flash_info_t info = { 0 };` 并检查 `get_info` 返回值。

#### 坑 11（重点）：行尾符（CRLF/LF）导致整文件 diff
- **现象**：改完 `main.c` 后 `git diff` 显示**整个文件**被"重写"（每行都 - 和 +），内容明明没变。
- **原因**：本仓库 git blob 存的是 **LF**；编辑器/写文件工具写入的是 **CRLF**（或反之），git 逐行比较认为全部不同。
- **诊断方法**：
  - `git diff --ignore-cr-at-eol --stat` → 若只剩真实逻辑改动，即可确认是 CR 行尾问题；
  - 注意 `git show HEAD:file | Out-String` 会被 PowerShell 管道转行尾，**不可靠**；用 `cmd /c "git show HEAD:file > out"` 原样重定向才准。
- **解决**：把改动文件统一转回 LF：
  ```powershell
  $c = [IO.File]::ReadAllText($f); $c = $c -replace "`r`n","`n"
  [IO.File]::WriteAllText($f, $c, (New-Object Text.UTF8Encoding($false)))
  ```
- **教训**：本项目所有源文件保持 **LF**，改动后都要检查 `git diff --stat`，别引入行尾污染。

#### 坑 12：4 字节地址命令（>16 MiB 器件）
- **现象**：w25q02jvm 是 256 MiB，3 字节地址（0x02/0x03）只能访问低 16 MiB。
- **解决**：驱动统一用 4 字节地址命令（READ4/PP4/ERASE4_*），并实测通过（见第五节）。

### C. 代码风格统一（用户要求）

- 用户工程风格：**K&R 花括号**（`void) {`）+ **注释前置独立行** + 无 `/* --- */` 分隔线块。
- 把 `spi_flash.c/.h` 全量统一：函数花括号改 K&R、赋值/宏/枚举/结构体成员的行尾注释全部前置、删除分隔线注释、清理多余空行。
- 注意：`#endif /* SPI_FLASH_H */` 的注释属于 header guard 惯例，保留；行尾保持 LF。

---

## 五、验证：如何确认实现是正确的（重点）

验证分四层，每层回答一个独立问题，构成完整证据链：

### 第 1 层：设备到底挂没挂上总线？（QEMU 侧）

- **手段**：HMP `info qtree` + `info block`。
- **证据**：
  ```
  dev: pl022, id ""
    bus: ssi
      type SSI
      dev: w25q02jvm, id ""
        drive = "mtd0"
        cs = 0 (0x0)
  ```
  flash 出现在 pl022 的 `bus: ssi` 下且 `drive="mtd0"`，同时 FPGAIO 有 `gpio-out "spi-cs" 1` → 设备挂载和 drive 绑定都正确。
- **陷阱提醒**：`info qom-tree` 里 flash 在 `/machine/unattached/device[0]` 是 QOM 对象树的正常表现，不代表没挂总线（见坑 4）。

### 第 2 层：CS 连接链路通不通？（FPGAIO MISC → flash CS）

- **手段**：QEMU `-trace enable=m25p80_select,m25p80_command_decoded` 启动，固件 toggle CS。
- **证据**：每次固件写 FPGAIO MISC bit8，trace 就出现 `m25p80_select ... select` / `... deselect` 边沿事件：
  ```
  m25p80_command_decoded ... new command:0x9f     # JEDEC 命令
  m25p80_select ... deselect                       # CS 释放
  m25p80_select ... select                         # CS 选中
  ```
  说明 `qdev_connect_gpio_out_named("spi-cs", 0, flash "ssi-gpio-cs")` 的连接真实生效，固件写寄存器能驱动到 flash 的 CS 引脚。
- **为什么这层重要**：m25p80 只有 CS 边沿才回 IDLE（坑 5）。能观察到 select/deselect 边沿，就证明"事务间状态机能复位"这一前提成立。

### 第 3 层：能不能真正读写？（固件实测）

- **手段**：固件 `spi_flash_selftest()` —— 擦扇区 0 → 读回应全 `0xFF` → 写 `i*3+1` 模式 → 读回逐字节比对。
- **证据**：
  ```
  spi_flash: JEDEC EF 70 22, size=256 MiB, page=256, sector=4096, 4B-addr=1
  spi_flash: selftest OK (pattern on sector 0)
  ```
- **为什么覆盖 4 字节地址**：selftest 内部走的就是 `ERASE4_4K(0x21)/PP4(0x12)/READ4(0x13)`，实测通过即证明 4 字节地址命令在 QEMU 的 `m25p80` 里正确实现（区别于 3 字节命令）。

### 第 4 层：代码/提交干不干净？

- **手段**：`cmake --build` 输出零 warning（仅 linker 原有的 `LOAD segment with RWX` 提示，与本次无关）；`git diff --stat` 干净。
- **证据**：
  - `spi_flash.c.obj` 编译无警告；
  - `git diff --ignore-cr-at-eol` 后 BareMetal `main.c` 完全无差异（内容已还原）、FreeRTOS `main.c` 仅 18 行新增；
  - 新增文件行尾 LF，`git status` 中为 `??`（未跟踪，正常）。

> 验证小结：**设备树（挂载）→ trace（CS 通）→ 固件（读写对）→ 编译（零警告）** 四层全过，才最终确认"这套 QEMU + 驱动 + 固件接入"是正确可用的。

---

## 六、构建与运行命令

> 💡 需要"生成 SPI flash 镜像"和"启动时挂载 SPI flash"的命令速查？见 `WORKLOG-2026-08-11-qemu-spi-flash-commands.md`（`qemu-img`/`flash_image.py` 生成镜像 + `-drive if=mtd` 挂载 + 辅助子命令 + 注意点）。

### QEMU 侧（重编译）

```powershell
$env:Path = "C:\Users\xidon\program\MSYS64\usr\bin;C:\Users\xidon\program\MSYS64\mingw64\bin;C:\Users\xidon\program\MSYS64\mingw64\lib\python3.12;" + $env:Path
cd C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-configure
ninja qemu-system-arm.exe
```

### 固件侧（FreeRTOS）

```powershell
$env:Path = "C:\Users\xidon\program\ARM_Gcc\gcc-arm-none-eabi-15.3-2026.06\bin;" + $env:Path
cd C:\Users\xidon\code\github\qemu-embedded-firmware
cmake -S . -B boards/mps2-an505/FreeRTOS/build-cmake -G Ninja `
  "-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake" `
  "-DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe" `
  -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DLWIP_OS=FreeRTOS
cmake --build boards/mps2-an505/FreeRTOS/build-cmake --target an505-qemu
```

### 运行（QEMU）

```powershell
cd C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-configure
# 先准备 flash 镜像（或用 works/tools/flash_image.py 创建/预置数据）
.\qemu-img.exe create -f raw C:\Users\xidon\code\github\qemu-embedded-firmware\boards\mps2-an505\BareMetal\build\flash.bin 256M
.\qemu-system-arm.exe -machine mps2-an505 -cpu cortex-m33 -m 16M -nographic `
  -kernel C:\Users\xidon\code\github\qemu-embedded-firmware\boards\mps2-an505\FreeRTOS\build-cmake\boards\mps2-an505\FreeRTOS\an505-qemu.elf `
  -drive if=mtd,format=raw,file=C:\Users\xidon\code\github\qemu-embedded-firmware\boards\mps2-an505\BareMetal\build\flash.bin
# 想看 CS 边沿 / 命令解码：
#   -trace enable=m25p80_select -trace enable=m25p80_command_decoded
```

---

## 七、后续维护约定

1. **QEMU 侧改动未提交**：`qemu-embedded-platform/qemu` 的 `mps2-tz.c`、`mps2-fpgaio.c/.h` 由用户自行 review 后提交。
2. **行尾必须 LF**：本项目源文件 git blob 均为 LF，改文件后务必检查 `git diff --stat`，工具写入若变 CRLF 需转回（见坑 11）。
3. **新增 `.c` 要重新 cmake 配置**：`Core/CMakeLists.txt` 用 `file(GLOB)`，glob 在配置期求值，加文件后需重新 `cmake -S . -B ...`（沿用 08-09 坑 5 的约定）。
4. **SPI flash 驱动**：`spi_flash.c/h` 放在 board 共享 `Core/Src`+`Core/Inc`，三个工程（BareMetal/FreeRTOS/threadx）的 CMake 都会 glob 到；后续对接文件系统时按头文件里的 LittleFS glue 示例实现 `read/prog/erase`。
5. **flash.bin 相关操作**：用 `works/tools/flash_image.py` 创建（全 0xFF 擦除态）/ 预置模式 / dump / 校验，避免手改大文件。
6. **验证三板斧**：改 QEMU 侧后重编 + `info qtree` + `-trace m25p80_select`；改固件后 `cmake --build` 零警告 + `selftest` 擦写读。
