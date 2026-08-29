# WORKLOG-2026-08-29：FreeSWITCH 自定义配置备份与还原

> **背景**：经哈希对比确认，整个 FreeSWITCH 调试中**真正改过的自定义文件只有 2 个**，
> 其余全是默认配置（重装自带）。已将这 2 个文件单独备份，安装新 FS 后按本文还原即可。

---

## 1. 备份内容

备份位置：`libutils/pjprojec/ports/freeswitch/`（保持与 FS conf 相同的相对目录结构）

| 备份文件 | 用途 / 关键配置 |
|---|---|
| `sip_profiles/internal-lo.xml` | **QEMU guest 专用 SIP profile**。绑定 tap0 网段 `172.16.23.1`，`context=default`（9196 echo 在 default dialplan），`auth-calls`，含 SRTP 协商参数 |
| `autoload_configs/acl.conf.xml` | **访问控制**。`domains` 列表放行 `172.16.23.0/24`（guest 网段），其余保持 FS 默认 |
| `dialplan/default.xml` | **拨号计划（已修复版）**。默认密码 sleep 已改为 `sleep 0`；否则新装 FS 后每通呼叫延时 10 秒。含 9196 echo 等默认 extension |

> 说明：分机 `directory/default/1000.xml`（guest）、`1005.xml`（手机）**不在备份内**——
> 它们是默认模板，重装会保留；若改动过再自行备份。

---

## 2. 还原步骤（安装新 FS 之后）

### 2.1 复制两个备份文件回 conf 目录（管理员 PowerShell）

```powershell
$src = 'C:\Users\xidon\code\github\qemu-embedded-firmware\libutils\pjprojec\ports\freeswitch'
$dst = 'C:\Program Files\FreeSWITCH\conf'

Copy-Item "$src\sip_profiles\internal-lo.xml"      "$dst\sip_profiles\" -Force
Copy-Item "$src\autoload_configs\acl.conf.xml"     "$dst\autoload_configs\" -Force
Write-Host 'restored'
```

### 2.2 校验（可选，确认放行节点在）

```powershell
Select-String -Path "$dst\autoload_configs\acl.conf.xml" -Pattern '172.16.23.0/24'
Select-String -Path "$dst\sip_profiles\internal-lo.xml" -Pattern 'sip-ip|rtp-ip'
```

### 2.3 重新加载配置

FS 运行中（手动 `FreeSwitchConsole.exe` 或服务）执行：

```powershell
& 'C:\Program Files\FreeSWITCH\fs_cli.exe' -H 127.0.0.1 -P 8021 -p ClueCon -x 'reloadxml'
```

> 若 `reloadxml` 不生效（profile 改动需重启才完全加载），重启 FS。

### 2.4 还原 dialplan 默认密码 sleep（必做）

新装 FS 后 `default.xml` 里 `default_password` 仍为默认 `1234` 时，**每通呼叫会先 `sleep 10000`（10 秒）**
（`dialplan/default.xml` 第 135 行 `<action application="sleep" data="10000"/>`）。两种还原方式任选：

**方式 A：用备份的已修复版直接覆盖**（带回完整 dialplan，含 9196 echo）：
```powershell
Copy-Item 'C:\Users\xidon\code\github\qemu-embedded-firmware\libutils\pjprojec\ports\freeswitch\dialplan\default.xml' 'C:\Program Files\FreeSWITCH\conf\dialplan\' -Force
```

**方式 B：只改 sleep 行、保留新版 dialplan**（管理员运行，把 `sleep 10000 → 0`，不改密码）：
```powershell
powershell -ExecutionPolicy Bypass -File 'C:\Users\xidon\code\github\qemu-embedded-firmware\works\tools\fs_remove_default_password_sleep.ps1'
```

> 方式 A 覆盖的是 v1.11.2 时代的完整 dialplan；若新版 FS 的 dialplan 有变化不想覆盖，用方式 B。
> 无论哪种方式，改完需 `reloadxml`（或重启 FS）。

### 2.5 核对分机

`conf/directory/default/1000.xml`（guest，密码 1234）与 `1005.xml`（手机）确认存在；
若新装后缺失/被重置，需重建（默认模板一般自带）。

---

## 3. 还原后验证

```powershell
# 1) 启动 QEMU guest（tap0），等注册
# 2) guest 注册成功：UDP 命令服务器 status 显示 Registered / Ping-Reachable
# 3) 拨号测试（应 <2s 建立）：
#    dial 9196 -> ACTIVE -> dtmf 1234 -> hangup
# 4) 调用方 fs_cli 确认:
& 'C:\Program Files\FreeSWITCH\fs_cli.exe' -H 127.0.0.1 -P 8021 -p ClueCon -x 'sofia status profile internal-lo'
#    应显示 guest 1000 Registered
```

---

## 4. 附注

- **备份原因**：`internal-lo.xml`、`acl.conf.xml` 在 v1.11.2 → v1.11.3 重装时被安装器**保留**，
  备份是为了更保险（防止将来完全卸载重装时丢失）；`dialplan/default.xml` 重装时**会被重置**
  （默认密码 sleep 10000 回归），因此单独备份**已修复版**（`sleep 0`）。
- SRTP：`internal-lo.xml` 内含 `inbound/outbound-srtp-negotiation=optional`；
  v1.11.x 的 SRTP 编译进 `core/mod_sofia`（无需 `mod_srtp` 模块）。
- 完整恢复思路（含网络/固件侧）见 `WORKLOG-2026-08-29-fs-reinstall-reconfig.md` 与
  `WORKLOG-2026-08-29-tap0-qemu-pjproject-setup.md`。
