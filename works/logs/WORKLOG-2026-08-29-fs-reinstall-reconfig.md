# WORKLOG-2026-08-29：FreeSWITCH 重装后重新配置清单

> **场景**：当前 FS（1.11.2 64bit）缺 `mod_srtp.dll`，需重装/换完整版。
> **配置备份**：经哈希对比确认，调试真正改过的自定义文件只有 **2 个**——
> `sip_profiles/internal-lo.xml`（自建 profile）和 `autoload_configs/acl.conf.xml`（放行网段），
> 重装时这 2 个都会被安装器保留；而 `dialplan/default.xml` 重装时**会被重置**
> （默认密码 sleep 10000 回归）。已把 3 个自定义文件（含 default.xml 已修复版）备份到
> `libutils/pjproject/ports/freeswitch/`，还原步骤见 `WORKLOG-2026-08-29-fs-config-restore.md`；
> 也可用 `works/tools/fs_remove_default_password_sleep.ps1` 只处理 sleep（见 1.5 节）。
> 重装会用默认配置覆盖，需按下述步骤**重新配置**。

---

## 0. 背景（当前环境的 FS 关键事实）

- FS 版本：1.11.2-release 64bit，以 `FreeSwitchConsole.exe` **手动运行**（非 Windows 服务）
- fs_cli：`C:\Program Files\FreeSWITCH\fs_cli.exe -H 127.0.0.1 -P 8021 -p ClueCon -x '<cmd>'`
  （**必须连 127.0.0.1**；连 172.16.23.1 会被 `loopback.auto` ACL 拒绝）
- 网络：tap0 独立网段（guest 172.16.23.50 ↔ 宿主 172.16.23.1），`internal-lo` profile 绑定 172.16.23.1
- 对端：Android 手机 1005 注册在 `internal` profile（0.0.0.0:5060）
- 分机：1000（guest）/ 1234、1005（手机），用户定义在 `conf/directory/default/`

---

## 1. 重装后必须重新做的配置

### 1.1 `conf/sip_profiles/internal-lo.xml`（QEMU guest 专用 profile）
> 重装后可能没有此文件，或为默认模板。**从备份复制**或按下述内容核对：
```xml
<profile name="internal-lo">
  <aliases/>
  <gateways/>
  <domains>
    <domain name="all" alias="true" parse="false"/>
  </domains>
  <settings>
    <param name="debug" value="0"/>
    <param name="sip-trace" value="no"/>
    <param name="sip-capture" value="no"/>
    <param name="context" value="default"/>          <!-- ⚠️ 必须 default（9196 echo 在 default dialplan） -->
    <param name="rfc2833-pt" value="101"/>
    <param name="sip-port" value="5060"/>
    <param name="dialplan" value="XML"/>
    <param name="dtmf-duration" value="2000"/>
    <param name="inbound-codec-prefs" value="$${global_codec_prefs}"/>
    <param name="outbound-codec-prefs" value="$${global_codec_prefs}"/>
    <param name="rtp-timer-name" value="soft"/>
    <param name="disable-rtp-auto-adjust" value="true"/>
    <param name="rtp-ip" value="172.16.23.1"/>        <!-- 宿主 tap0 -->
    <param name="apply-nat-acl" value="deny.auto"/>
    <param name="sip-ip" value="172.16.23.1"/>        <!-- 宿主 tap0 -->
    <param name="hold-music" value="$${hold_music}"/>
    <param name="local-network-acl" value="localnet.auto"/>
    <param name="apply-inbound-acl" value="domains"/>
    <param name="inbound-late-negotiation" value="true"/>
    <param name="nonce-ttl" value="60"/>
    <param name="auth-calls" value="$${internal_auth_calls}"/>
    <param name="auth-subscriptions" value="true"/>
    <param name="inbound-reg-force-matching-username" value="true"/>
    <param name="auth-all-packets" value="false"/>
    <param name="ext-rtp-ip" value="172.16.23.1"/>
    <param name="ext-sip-ip" value="172.16.23.1"/>
  </settings>
</profile>
```

### 1.2 `conf/autoload_configs/acl.conf.xml`（放行 guest 网段）
在 `domains` 列表加：
```xml
<list name="domains" default="deny">
  <node type="allow" domain="$${domain}"/>
  <!-- QEMU guest over tap0 segment (172.16.23.50). -->
  <node type="allow" cidr="172.16.23.0/24"/>
</list>
```

### 1.3 `conf/vars.xml`（domain 变量）
- 默认 `domain = $${local_ip_v4}`（宿主机 LAN IP，本机 192.168.23.7）——保持默认即可
- 若 FS 检测的主 IP 变了，注意 guest 固件 `PJ_PHONE_DIAL_HOST` 也要一致

### 1.4 分机账号
- `conf/directory/default/1000.xml`：guest 分机 1000（密码 1234）——从备份复制或重建
- `conf/directory/default/1005.xml`：手机分机 1005——从备份复制或重建

### 1.5 dialplan（9196 echo）
- `conf/dialplan/default.xml` 含 `<extension name="echo"> destination_number ^9196$`（默认自带）
- 若缺失，从备份恢复

> ⚠️ **默认密码 sleep 坑（重装必查！实测 v1.11.3）**：`default.xml` 的 "global" extension 有段安全提醒逻辑——
> 只要 `vars.xml` 里 `default_password` 还是默认 `1234`，**每通呼叫都会先 `sleep 10000`（10 秒）**。
> 重装/升级恢复默认配置后此逻辑会**复活**，表现为拨号后 guest 停在 DIALING ~10-15s 才 ACTIVE
> （v1.11.2 里已用脚本去掉，升级后回归）。
> 修复：管理员跑 `works/tools/fs_remove_default_password_sleep.ps1`（`sleep 10000 → 0` + reloadxml），
> 或手动把 `default.xml` 的 `data="10000"` 改成 `data="0"` 后 `reloadxml`。**不要改 `default_password`**（会影响注册）。
> 实测修复后呼叫建立：~15s → **0.8s**，冒烟回归 3/3 PASS。

### 1.6 可选：SRTP 配置（本次重装目的）
拿到 `mod_srtp.dll` 后：
```xml
<!-- modules.conf.xml -->
<load module="mod_srtp"/>
<!-- internal-lo.xml settings 加 -->
<param name="inbound-srtp-negotiation" value="optional"/>
<param name="outbound-srtp-negotiation" value="optional"/>
```

---

## 2. 验证步骤（重装+配置后）

```powershell
# 1) 启动 FS（手动）
C:\Program Files\FreeSWITCH\FreeSwitchConsole.exe

# 2) 确认 internal-lo 监听
netstat -ano | findstr 5060        # 应见 172.16.23.1:5060

# 3) 确认 guest 注册
& 'C:\Program Files\FreeSWITCH\fs_cli.exe' -H 127.0.0.1 -P 8021 -p ClueCon -x 'sofia status profile internal-lo reg'
# 应见: 1000@192.168.23.7 Registered, Contact=sip:1000@172.16.23.50:15062, Ping-Status Reachable

# 4) 拨号 9196（echo）验证媒体
powershell -ExecutionPolicy Bypass -File works\tools\run_smoke_regression.ps1 -Iterations 3
```

---

## 3. 注意事项

1. **fs_cli 连 127.0.0.1**（`loopback.auto` 只放行回环）
2. **`internal-lo` 的 context 必须是 `default`**，否则 9196 echo 拨不通（480）
3. 手机 1005 走 `internal` profile（0.0.0.0:5060），重装后确认 internal.xml 监听正常
4. 重装后 `local_ip_v4`（FS 主 IP）可能变化——若变，需同步改 guest 固件的 `PJ_PHONE_DIAL_HOST`（编译）或运行时 `host` 命令
5. 自定义配置备份在 `libutils/pjproject/ports/freeswitch/`（3 个文件：`internal-lo.xml`、`acl.conf.xml`、`dialplan/default.xml` 已修复版），还原步骤见 `WORKLOG-2026-08-29-fs-config-restore.md`
