# 2026-08-11 工作记录：littlefs/fatfs 文件系统接入 + FatFS 对接 SPI flash 测试

> 本文档整理 2026-08-11 在 `qemu-embedded-firmware` 中完成的工作：把两个文件系统（littlefs、FatFS）源码接入工程，用现有 SPI flash 驱动（`spi_flash.c`）在 `ports/` 内写对接层，并完成 FatFS 测试代码与 QEMU 实测验证。**重点记录遇到的困难与解决思路、以及排查一个隐蔽 bug 的完整方法论**。
>
> 前置依赖（昨天/前天成果）：QEMU 的 `mps2-an505` 已挂载 256 MiB 外部 SPI NOR（w25q02jvm），固件已有 `boards/mps2-an505/Core/Src/spi_flash.c` 工业级驱动。

---

## 目录

- [一、工作总览](#一工作总览)
- [二、功能实现清单](#二功能实现清单)
- [三、遇到的困难与解决思路（重点）](#三遇到的困难与解决思路重点)
- [四、Bug 排查方法论：FatFS 写不落盘（重点）](#四bug-排查方法论fatfs-写不落盘重点)
- [五、验证](#五验证)
- [六、构建与运行命令](#六构建与运行命令)
- [七、维护约定](#七维护约定)

---

## 一、工作总览

| 环节 | 内容 |
|------|------|
| littlefs 接入 | 新增 `libutils/littlefs/CMakeLists.txt` + `ports/spi_flash/lfs_port_spi_flash.{c,h}` |
| fatfs 接入 | 新增 `libutils/fatfs/CMakeLists.txt` + `ports/spi_flash/diskio_spi_flash.{c,h}` + `ports/spi_flash/ffconf.h`（配置覆写） |
| 接线 | `libutils/CMakeLists.txt` 加入两个子目录；FreeRTOS 应用链接 `littlefs`/`fatfs` |
| 测试 | `boards/mps2-an505/FreeRTOS/application/fatfs_test.{c,h}`，`main.c` 接入调用 |
| 验证 | QEMU 首次启动（格式化）与二次启动（挂载已有卷）均 PASSED，卷跨重启持久化 |

**踩到的一个大坑**：QEMU 的 m25p80 模型会强制 NOR「先擦后写」语义，导致 FatFS 的目录/FAT 更新静默写不进去。最终通过"逐层隔离 + 最小对照实验 + 绕过缓存拿 ground truth"定位，并修复 `disk_write` 为擦前写。

---

## 二、功能实现清单

### 1. littlefs（上游 v2.11，无 CMake）

- `libutils/littlefs/CMakeLists.txt`：静态库 `littlefs`（`lfs.c` + `lfs_util.c` + 端口文件）。
- `libutils/littlefs/ports/spi_flash/lfs_port_spi_flash.{c,h}`：`read/prog/erase/sync` → `spi_flash_read/write/erase_sector/sync`；块大小 = 4 KiB 扇区、whole-flash 卷（256MiB/4K = 65536 块）；文件级静态缓存（单实例）；`LFS_THREADSAFE` 时 lock/unlock → `spi_flash_os_lock/unlock`。API：`lfs_spi_flash_config_init/mount/format`，mount 懒初始化 spi_flash。
- littlefs 无需处理"擦前写"：littlefs 保证先调用 erase 回调再 prog。

### 2. fatfs（上游 R0.16，无 CMake）

- `libutils/fatfs/CMakeLists.txt`：静态库 `fatfs`（`source/ff.c` + `ffsystem.c` + `ffunicode.c` + 端口文件）；**不编译 `source/diskio.c` 模板**（否则重名符号）。
- `libutils/fatfs/ports/spi_flash/diskio_spi_flash.{c,h}`：`disk_*` → `spi_flash_*`；512B 逻辑扇区；`GET_SECTOR_COUNT/SIZE`、`GET_BLOCK_SIZE=8`、`CTRL_SYNC`、`CTRL_TRIM`（按 4K 擦）。
- `libutils/fatfs/ports/spi_flash/ffconf.h`：项目配置（覆写上游，见困难 2）。
- `libutils/fatfs/ports/spi_flash/diskio_spi_flash.c` 的 `disk_write`：**擦前写**（见 Bug 章节）。

### 3. 测试与应用接入

- `boards/mps2-an505/FreeRTOS/application/fatfs_test.{c,h}`：mount → 无 FAT 则 chip erase + `f_mkfs` → `f_getfree` → `f_mkdir` → 写/读回 83B 文件 → 列目录 → `f_unmount` → PASSED。
- `boards/mps2-an505/FreeRTOS/application/main.c`：`main_task_entry` 在 `spi_flash_init` 后调用 `fatfs_test()`。

---

## 三、遇到的困难与解决思路（重点）

### 困难 1：两个文件系统上游都没有 CMake 构建

**思路**：不动上游源码，各自补 `CMakeLists.txt`；把"上游源文件"与"对接层"分离——对接代码全部放 `ports/`，便于升级时只替换上游目录。

**小坑**：littlefs 没有 `lfs_config` typedef，API 用的是 `struct lfs_config`。编译报 `unknown type name 'lfs_config'` 后立刻意识到，全部改成 `struct lfs_config`。

### 困难 2：用户要求"不修改上游 ffconf.h，配置在 ports 内覆写"

**关键约束**：C 预处理器对 `#include "ffconf.h"`（引号形式）按"**当前文件所在目录优先**"查找。`ff.c`/`ff.h` 在 `source/` 目录里，所以单纯加 `-I ports` 根本盖不住 `source/ffconf.h`——ff.h 内部的 `#include "ffconf.h"` 永远先找到 source 目录那份。

**解决思路（读源码找钩子，而不是猜）**：读 `source/ff.h` 发现 R0.16 自带覆写钩子：

```c
#if !defined(FFCONF_DEF)
#include "ffconf.h"		/* FatFs configuration options */
#endif
```

于是用 CMake 对该库所有 TU 强制预包含我们的配置：

```cmake
target_compile_options(fatfs PUBLIC
    "-include;${CMAKE_CURRENT_SOURCE_DIR}/ports/spi_flash/ffconf.h"
)
```

`-include` 最先执行，先定义了 `FFCONF_DEF`，于是 ff.h 跳过上游 `ffconf.h` → **上游 `source/ffconf.h` 保持 pristine，配置全部落在 `ports/spi_flash/ffconf.h`**（从上游拷贝后只改 `FF_USE_MKFS=1 / FF_USE_LFN=1 / FF_CODE_PAGE=437 / FF_USE_TRIM=1 / FF_FS_NORTC=1`）。

**ABI 一致性**：`FF_USE_LFN` 等会影响 `FIL`/`FATFS` 结构体布局。若只在库内生效、应用 TU 看到的是默认配置，会布局错位。所以用 `PUBLIC` 传播 `-include` 给链接 `fatfs` 的应用目标（验证过 build.ninja 中应用 TU 也带上了该参数）。

### 困难 3：R0.16 的 `f_mkfs` 签名变了

编译报"expected 4 arguments, have 5"（旧版 `f_mkfs(path, opt, au, work, len)`）。读 `ff.h` 确认新版是：

```c
FRESULT f_mkfs (const TCHAR* path, const MKFS_PARM* opt, void* work, UINT len);
```

改用 `MKFS_PARM mopt = {0}; mopt.fmt = FM_FAT;`。

### 困难 4：无 RTC，链接报 `get_fattime` 未定义

`FF_FS_NORTC=0` 时 FatFS 需要用户提供 `get_fattime()`。固件无 RTC，正确做法是配置 `FF_FS_NORTC=1`（用 `FF_NORTC_*` 固定时间戳），而不是写死一个假时间函数。

---

## 四、Bug 排查方法论：FatFS 写不落盘（重点）

### 现象

QEMU 首次跑测试：`f_mkfs / f_mount / f_mkdir / f_open(w) / f_write / f_close` **全部返回 OK**，但紧接着 `f_open(r)` 返回 `FRESULT=5`。

### 第一步：先搞清错误码含义，不猜

读 `ff.h` 的 `FRESULT` 枚举 → `5 = FR_NO_PATH`（"找不到路径"），即 `SPIFS` 目录在盘上不存在。**问题不是"文件没找到"，而是"目录没写进去"**——方向立刻收敛到"写"。

### 第二步：给测试加诊断，观察状态变化的时间点

- 打印 FRESULT 名字；在 `f_mkdir` 后 / `f_write` 后 / `f_close` 后分别列根目录。
- 发现：`f_mkdir` 之后 `f_opendir("")` **能看到 SPIFS**（因为 FatFS 的 fs->win 窗口缓存里是脏数据），但 `f_close` 之后根目录为空。

### 第三步：绕过缓存拿 ground truth

FatFS 有内部扇区窗口缓存，光靠 `f_opendir` 无法区分"缓存里有"还是"盘上有"。于是用 `disk_read` **直接读根目录扇区**（LBA=pfs->database，绕开 FatFS 缓存）打印原始 hex → 全是 `00`。**结论：目录写根本没落到 flash**，只是停在 RAM 缓存里。

### 第四步：找"矛盾点"并做最小对照实验

一个关键矛盾：`f_mkfs` 的写能落盘（二次启动能 `f_mount` 上卷），但目录/文件写不能。于是加了一段**绕过 FatFS 的裸驱动自检**：直接在 `0x200000`/`0x800000` 用 `spi_flash_write` 写 `A5`/`A6` 再读回。

对照结果非常关键：
- 在**已擦除（0xFF）**的 flash 上：写→读回 `A5/A6` ✅
- 在**全新全零（0x00）**的 flash 上：写→读回 `00 00` ❌

→ 锁定根因：**QEMU 的 m25p80 模型强制 NOR「先擦后写」语义**（page program 是对旧数据按位 AND，只能 1→0）。对未擦除区域写数据 = 静默失败。`f_mkfs` 之所以"看似正常"，是因为它恰好只写刚被 chip erase 清成 0xFF 的区域；而目录/FAT 的**二次更新**落在已写过（非 0xFF）的扇区上，直接写就丢数据。

### 第五步：修复

`libutils/fatfs/ports/spi_flash/diskio_spi_flash.c` 的 `disk_write` 改为**擦前写**：
- 目标 512B 扇区全 `0xFF` → 直接编程（常见路径，快）；
- 否则读整 4 KiB flash 扇区 → 改 512B → `spi_flash_erase_sector` → 整体写回（read-modify-erase-write）。

### 插曲：我自己埋的诊断 bug

排查中加了一段打印卷布局的诊断，用了**在 `f_getfree` 之前尚未初始化的 `pfs` 指针**，导致固件挂死。吸取教训：**诊断代码同样要小心，打印前确认指针已赋值**。

### 方法论小结

1. **先翻译错误码/读上游源码**，把现象精确化，不凭旧经验猜（R0.16 改了 API）。
2. **逐层隔离**：应用层（FatFS）→ 端口层（disk）→ 驱动层（spi_flash）→ 模型层（m25p80），用"绕过上一层"的方式定位在哪一层。
3. **绕过缓存拿 ground truth**：直接读原始扇区，别信上层缓存/返回值。
4. **找矛盾点做最小对照实验**：能写 vs 不能写的差异（已擦除 vs 未擦除）就是答案。
5. 驱动自带 `selftest` 只测扇区 0，测不出这类地址/状态相关问题——**单点验证 ≠ 全路径验证**。

### 附：为什么必须"先擦后写"——NOR Flash 通用原理

> 以下为通用知识，适用于**所有 SPI NOR Flash**（Winbond w25q、Macronix MX25、GigaDevice GD25、ISSI IS25、Micron N25Q/MT25Q、Infineon/Cypress S25FL 等），**不是 m25p80 特有，也不是 QEMU 的模拟设定**；真实芯片同样如此。

- **物理原理**：NOR 单元靠浮栅电荷表示 0/1。编程（Program）只能把 bit 从 `1→0`（写电荷/清位）；擦除（Erase）把**整块**恢复成 `1`（0xFF）。"把某个 bit 从 0 改回 1"没有编程命令能做到，必须先擦整块。
- **关键区分**：不是"每次写都要先擦"。只要一次编程只做 `1→0` 的位清零，同一块内可反复编程（例如写标志位）；只有需要把某 bit 从 `0` 变回 `1` 时才必须先擦。`disk_write` 里"先读旧数据、按 `(new & ~old)` 判断是否需要擦"正是利用这个特性。
- **芯片间差异不在"要不要擦"，而在**：擦除粒度（4K/32K/64K，命令 `0x21/0x5c/0xdc`）、页编程大小（普遍 256B）、地址模式（3/4 字节，>16MiB 必须 4 字节命令）、命令码与状态寄存器布局。所以驱动里"擦除粒度/页大小/地址宽度"按芯片配置，而"先擦后写"这条铁律通用。
- **QEMU m25p80 模型**：page program 实现为 `storage[i] &= data[i]`，是**忠实模拟真实硬件**。因此这次 `disk_write` 的擦前写修复**不是"为了骗过 QEMU"，而是真机上本来就该这么做**。
- **对文件系统设计的意义**：
  - FatFS 假设介质可随意覆写（面向磁盘/MMC），NOR 不行 → 磁盘对接层的"擦前写"本质上是一个**极简 FTL（闪存转换层）**。
  - 当前 RMW（写 512B 前擦 4K 再整块写回）正确但偏慢；真机/量产可优化：扇区写入合并、加小缓存延迟擦除、配合 `CTRL_TRIM` 批量擦除。
  - littlefs 原生为 NOR 设计（自带"先擦后写 + 磨损均衡 + 掉电保护"），所以 littlefs 端口无需处理擦除——这也是两类文件系统的本质差异。
  - 补充：SPI NAND（如 W25N 系列）同样"先擦后写"，但擦除块更大（128K/256K）、有坏块管理 + ECC，属另一体系。

---

## 五、验证

| 场景 | 结果 |
|------|------|
| 编译 | `liblittlefs.a`/`libfatfs.a` 零错误，`an505-qemu.elf` 链接通过 |
| 首次启动（全新全零 flash） | chip erase → f_mkfs → 写/读回 83B → 列目录 → **PASSED** |
| 二次启动（同一 flash.bin） | 挂载已有卷（`f_mkdir` 返回 already exists）、读回一致 → **PASSED**，卷跨重启持久化 |
| 原始扇区 | 根目录扇区含有效 `.` / `..` / `SPIFS` / `hello.txt` 条目 |

---

## 六、构建与运行命令

```powershell
# 配置（PowerShell 下含点的 -D 参数必须加引号）
cmake -S . -B boards/mps2-an505/FreeRTOS/build-cmake -G Ninja `
  "-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake" `
  "-DCMAKE_MAKE_PROGRAM=C:/Users/xidon/program/Ninja/ninja.exe" `
  -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DLWIP_OS=FreeRTOS

# 编译
cmake --build boards/mps2-an505/FreeRTOS/build-cmake

# 首次建 flash 镜像（256 MiB 全零）并运行
$fb = Join-Path $env:TEMP 'an505-flash.bin'
fsutil file createnew $fb 268435456
& 'C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-build\qemu-system-arm.exe' `
  -machine mps2-an505 -cpu cortex-m33 -m 16M -nographic `
  -kernel boards/mps2-an505/FreeRTOS/build-cmake/boards/mps2-an505/FreeRTOS/an505-qemu.elf `
  -drive if=mtd,format=raw,file=$fb
```

---

## 七、维护约定

- **改 FatFS 配置** → 只改 `libutils/fatfs/ports/spi_flash/ffconf.h`，不要碰上游 `source/ffconf.h`。
- **改 FatFS 磁盘层** → `libutils/fatfs/ports/spi_flash/diskio_spi_flash.c`；注意 `disk_write` 必须"擦前写"（NOR 语义）。
- **改 littlefs 对接** → `libutils/littlefs/ports/spi_flash/`。
- 上游 `source/` 保持 pristine，升级文件系统时只替换上游目录，`ports/` 与 `ffconf.h` 保留。
- 新建/改动文件用 CRLF 行尾（Windows + autocrlf=true）。
