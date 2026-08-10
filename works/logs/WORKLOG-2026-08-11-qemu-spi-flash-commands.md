# 2026-08-11 工作记录：QEMU SPI flash 镜像生成与挂载命令速查

> 本文档是 **命令速查**：记录在 QEMU 中如何**生成 SPI flash 镜像**（`qemu-img` / `works/tools/flash_image.py`），以及 QEMU **启动时如何挂载 SPI flash**（`-drive if=mtd`）。针对 `mps2-an505` 机器的外部 SPI NOR flash（Winbond `w25q02jvm`，256 MiB，JEDEC `EF 70 22`）。
>
> 完整实现思路、坑与验证见 `WORKLOG-2026-08-10-mps2-spi-flash.md`（08-10 已打通 挂载 → 驱动 → 擦写读 全链路）。本日志只聚焦"两条命令"及其注意点，方便日后直接翻阅。

---

## 目录

- [一、前置条件](#一前置条件)
- [二、生成 SPI flash 镜像的命令](#二生成-spi-flash-镜像的命令)
- [三、QEMU 启动时挂载 SPI flash 的命令](#三qemu-启动时挂载-spi-flash-的命令)
- [四、镜像辅助命令（flash_image.py）](#四镜像辅助命令flash_imagepy)
- [五、注意事项与常见问题](#五注意事项与常见问题)
- [六、相关日志与工具](#六相关日志与工具)

---

## 一、前置条件

| 项 | 值 |
|----|----|
| 机器 | `mps2-an505`（QEMU 自编译版 v11.1.0-rc3，仓库 `qemu-embedded-platform/qemu`） |
| flash 器件 | Winbond `w25q02jvm`，256 MiB，JEDEC `EF 70 22` |
| 挂载位置 | PL022 `spi0`（MMIO `0x40205000`）的 `"ssi"` 总线；CS 由 FPGAIO MISC bit8 控制 |
| 可执行文件 | `qemu-system-arm.exe`、`qemu-img.exe` 位于 `C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-configure` |
| QEMU 侧改动 | 必须先有 08-10 的 `mps2-tz.c`/`mps2-fpgaio.c` 改动，否则 `-drive if=mtd` **挂不上 flash**（见第五节问题 1） |

---

## 二、生成 SPI flash 镜像的命令

### 方式一：`qemu-img create`（QEMU 自带，快速）

```powershell
cd C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-configure
.\qemu-img.exe create -f raw C:\path\to\flash.bin 256M
```

> ⚠️ 注意：`qemu-img create` 生成的原始文件内容是**全 0x00**，**不是** NOR flash 的擦除态（0xFF）。若希望镜像默认是"已擦除"状态，请用方式二。

### 方式二：`flash_image.py create`（推荐，默认全 0xFF 擦除态）

```powershell
cd C:\Users\xidon\code\github\qemu-embedded-firmware
python works/tools/flash_image.py create flash.bin                  # 256 MiB，全 0xFF
python works/tools/flash_image.py create flash.bin --size 64K       # 指定大小（256M/64K/1234…）
```

- 尺寸必须 ≥ 器件地址空间所需；w25q02jvm 为 256 MiB，通常直接建满 `256M`。
- 全 0xFF 擦除态与 QEMU `m25p80` 在未绑定 drive 时的默认行为一致，最贴近真实 NOR 初始状态。

---

## 三、QEMU 启动时挂载 SPI flash 的命令

```powershell
cd C:\Users\xidon\code\github\qemu-embedded-platform\qemu\qemu-configure
.\qemu-system-arm.exe -machine mps2-an505 -cpu cortex-m33 -m 16M -nographic `
  -kernel C:\path\to\an505-qemu.elf `
  -drive if=mtd,format=raw,file=C:\path\to\flash.bin
```

- **`-drive if=mtd,format=raw,file=<镜像>`** 就是挂载 SPI flash 的关键参数：它会创建一个名为 `mtd0` 的 BlockBackend，QEMU 机器代码（`mps2-tz.c` 的 `make_spi()`）会把 `w25q02jvm` 的 `drive` 属性指向它。
- 验证挂载是否生效：QEMU 启动后进 HMP（`-nographic` 下 `Ctrl+A C`）执行 `info qtree`，应能看到：

  ```
  dev: pl022, id ""
    bus: ssi
      type SSI
      dev: w25q02jvm, id ""
        drive = "mtd0"
        cs = 0 (0x0)
  ```

- 需要观察 flash 命令 / CS 边沿时，追加 trace：

  ```powershell
  .\qemu-system-arm.exe ... -trace enable=m25p80_select -trace enable=m25p80_command_decoded
  ```

---

## 四、镜像辅助命令（flash_image.py）

镜像生成之后，日常还需要预置数据 / 查看 / 校验。`works/tools/flash_image.py` 提供如下子命令：

```powershell
cd C:\Users\xidon\code\github\qemu-embedded-firmware

# 预置数据
python works/tools/flash_image.py pattern flash.bin 0x100000 1M     # 写入 i*3+1 模式（与固件 selftest 一致）
python works/tools/flash_image.py zeros  flash.bin 0x100000 1M      # 写入 0x00

# 查看
python works/tools/flash_image.py info  flash.bin                   # 文件大小/首字节
python works/tools/flash_image.py dump  flash.bin 0x100000 256      # hex dump 某区域

# 校验
python works/tools/flash_image.py verify flash.bin 0x0 4096 --erased        # 期望全 0xFF（擦除态）
python works/tools/flash_image.py verify flash.bin 0x100000 1M --pattern    # 期望 i*3+1 模式
```

> 约定：改镜像用脚本、避免手改大文件；`pattern` 模式的字节算法 `(i*3+1)&0xFF` 与固件 `spi_flash_selftest()` 完全一致，宿主机侧可据此校验设备侧读回的数据。

---

## 五、注意事项与常见问题

1. **只给 `-drive if=mtd` 挂不上 flash（最常见误解）**
   - `-drive` 只创建 BlockBackend（后端存储）；必须由 QEMU 机器代码把 `m25p80`/`w25q02jvm` 实例化、接到 `"ssi"` 总线、并把 `drive` 属性指向该后端，flash 才会出现在总线上。
   - 只靠启动参数做不到（`-global` 只能覆盖已存在设备的属性，不能凭空挂设备）。详见 08-10 日志坑 2。
2. **镜像大小与器件一致**：`m25p80` 后端容量由 `file=<镜像>` 的文件大小决定。`w25q02jvm` 是 256 MiB，建议建满 `256M`；过小会限制可访问地址空间。
3. **擦除态差异**：`qemu-img create` → 全 0x00；`flash_image.py create` → 全 0xFF（NOR 擦除态）。需要哪种按需选择。
4. **改动镜像后要重启 QEMU**：`-drive` 是文件后端，重启（或先关后开）才重新读取镜像内容。
5. **固件每笔事务必须 toggle CS**：`m25p80` 状态机只在 CS 释放（deassert）时回 IDLE，CS 常选会导致后续命令错乱。固件驱动已通过 FPGAIO MISC bit8（0 选中 / 1 释放）处理，见 08-10 日志坑 5。
6. **行尾约定**：本机 Windows + `core.autocrlf=true`，新建/改动文件一律用 **CRLF** 结尾（本日志即 CRLF），否则 git 会误报"已修改"。

---

## 六、相关日志与工具

- 完整实现（QEMU 改动、固件驱动、坑与验证）→ `works/logs/WORKLOG-2026-08-10-mps2-spi-flash.md`
- 镜像工具脚本 → `works/tools/flash_image.py`（用法见其文件头注释与本日志第二/四节）
- 音频/网卡历史日志 → `works/logs/WORKLOG-2026-08-08-lan9118-lwip.md`、`WORKLOG-2026-08-09-mpsx-audio.md`
