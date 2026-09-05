# WORKLOG 2026-09-05 — tracer 日志集成三条遗留收尾

> 承接 WORKLOG-2026-09-03-tracer-log-interface.md 结尾列出的"待办（可选）"前三条。
> 范围经 vscode_askQuestions 确认：第 1 条**仅迁移**（不加 LVGL 日志页/落 flash），验证到 **QEMU 实跑**。

## 1. pj_phone.c 状态机事件 `tracer_ring_printf` → `TRACER_LOG*`（11 处全迁）
事件从"崩溃事件（无级别，`[<ms>] phone: ...`）"改为**带级别日志**，统一走 sink/drain 管道、带
`[<ms>][X]` 前缀、受运行期分级控制。级别选择对齐既有语义（正常/状态路径 INFO，可恢复重试 WARN）：
- `LOGI`：registered / incoming / call setup / call ACTIVE / call end / media active / dial /
  answer / reject / hangup
- `LOGW`：reg FAIL（pjsua 会自动重试）
- 全部去掉显式 `\r\n`（tracer_log 自动补 CRLF）。
- 结果：pj_phone.c **零 `tracer_ring_printf`、零裸 `printf`**。

## 2. phone_net.c（UDP cmd server）printf → tracer
- `socket()/bind()` 失败 → `TRACER_LOGE`；server up → `TRACER_LOGI`；每命令回显 `cmd '%s' -> %s` →
  `TRACER_LOGI`；`#include "tracer.h"`。
- 顺带修**既有** `-Wformat-truncation`：`snprintf(tmp[128], "%s", line)`（line 最长 199B）→
  `strncpy(tmp, line, sizeof(tmp)-1)` + 强制 NUL（strtok 需 NUL 终止）。
- 结果：phone_net.c 零裸 `printf`。

## 3. pjsua_init 窗口日志进 tracer（关键机制 = `log_cfg.cb`，零改 pjsua）
**根因**：`pjsua_init()` 内部 `pjsua_reconfigure_logging() → pj_log_set_log_func(&pjsua 静态 log_writer)`
会把我们装的全局 writer 覆盖掉；pjsua 的 `log_writer` 逻辑是——若 `pjsua_var.log_cfg.cb` 非空则回调该
cb，否则 `pj_log_write()`（→ 默认 printf）。
**修复（两处，全时段覆盖）**：
1. `pjsua_create()` **之前**就 `pj_log_set_log_func(&phone_pjlog_writer)` —— 覆盖 create + init 前段
   （init 内部 reconfigure 覆盖前的日志）。
2. 把 `phone_pjlog_writer` 注册为 `log_cfg.cb`（签名 `(int level, const char *data, int len)` 与
   `pjsua_logging_config.cb` 完全一致）—— init 内部 reconfigure **之后**（writer 已被 pjsua 覆盖）的日志
   经 pjsua log_writer 回调我们的 cb 回流 tracer（含 "pjsua version ... initialized" 横幅）。
3. `pjsua_init OK` 后再 `pj_log_set_log_func(...)` 一次夺回全局 writer（原逻辑保留）。

## 验证（QEMU 实跑 35s，user 网卡、无 FreeSWITCH）
- 构建：`cmake --build build-phone` 零编译告警（仅既有 linker `RWX permissions` 提示）。
- 第 3 条证据：pjsua init 横幅由上一轮裸的
  `00:00:08.408   pjsua_core.c  .pjsua version 2.17 for  initialized`
  变为本次带前缀的
  `[8394][I] 00:00:08.394   pjsua_core.c  .pjsua version 2.17 for  initialized`。
- 第 2 条证据：`[23211][I] phone_net: UDP cmd server on :15000`（原先裸 printf）。
- 全程无裸 `phone:` 无级别事件；codec/transport(TLS)/account/watchdog 等 `pj_phone:` 日志统一
  `[N][I/W]` 前缀；`wd: reg keepalive probe` 等正常。
- 无锁死，任务水位正常（mpsx_cap/play hwm≈7731/8132）。

## 范围外观察（本次未动，另开任务）
- main.c 的 PJLIB FreeRTOS port self-test 日志（`PJ THREAD NOT REGISTERED` / `os_core_freert` /
  `pj_test:`）与 mpsx 驱动（lcd/audio/mic/TOUCH）裸输出早于 pj_phone_init 的 writer 安装点，仍在 tracer
  管道外；要 0-printf 推广到这些需在 main.c 更早期装 PJ_LOG writer 或迁移驱动 printf。
- 09-03 遗留第 4 条（stm32 板接入 tracer 分级日志）仍未做。
- 09-03 遗留："运行日志上屏 + 异步落 flash 演示"（本次按用户选择只做了迁移，UI/flash 另开）。

## 4. SDES-SRTP 强随机 key（消除 "simple random generator" 警告，2026-09-05）
**现象**：媒体建流时大量重复 `[N][I] transport_srtp_sdes .Warning: simple random generator is used for
generating SRTP key`（每次本地构造 crypto attr 一条 = 编译进 suite 数 × 协商次数）。

**根因**：`pjmedia/src/pjmedia/transport_srtp_sdes.c` `generate_crypto_attr_value()` —— 本地 SDES key 未
提供（`crypto->key.slen==0`）时，按后端选随机源：OpenSSL→`RAND_bytes`、Apple→`SecRandomCopyBytes`、**其它
（本工程=mbedtls 4.2 后端）→ else 分支用 `pj_rand()`（标准库 rand 薄封装）+ 时间戳逐字节拼 key 并打
PJ_LOG(3) 警告**。弱随机 = 工业级 SRTP 弱点。

**触发链**：acc `srtp_opt.crypto_count=0` → `pjmedia_transport_srtp_create` 用
`pjmedia_srtp_enum_crypto()` 填全部编译进的 suite（本工程 libsrtp 2.5.0 纯软件 → 只有
AES_256_CM_HMAC_SHA1_80/_32 + AES_CM_128_HMAC_SHA1_80/_32 = **4 个**，实测吻合）→ sdes offer/answer 为每
个无 key suite 生成一次弱 key。

**修复（应用层预置强 key，零改 pjproject）**：
- `pj_crypto.c/h` 新增 `cred_random_bytes(uint8_t*,size_t)`：`psa_crypto_init()`+`psa_generate_random()`
  （mbedtls PSA RNG = TLS 握手同源熵，含 mbedtls_platform_get_entropy）。
- `pj_phone.c` 新增 `phone_srtp_key_len()`（镜像 crypto_suites cipher_key_len：AES_CM_256=46、
  AEAD_256_GCM=44、AES_CM_192=38、AEAD_128_GCM=28、AES_CM_128=30，均为 key+salt）+ `phone_prekey_srtp()`
  （`pjmedia_srtp_enum_crypto` 枚举 → 每 suite 生成 klen 字节强 key 填 `acc_cfg.srtp_opt.crypto[]`）→ 在
  `pjsua_acc_add` 前调用。有 key 后 sdes 跳过自生成（无警告 + 协商用强 key），pjsua_media.c 会把
  acc srtp_opt.crypto 原样拷进 SRTP setting（无需改 pjsua）。
- **验证**：构建零新告警；QEMU 实跑 `[22370][I] pj_phone: srtp pre-keyed 4 crypto suite(s)`（PSA 随机成功，
  count=4 与编译 suite 数吻合），启动 clean。**遗留**：FS 真实通话环境的"警告消失 + AES_256 协商仍 200"待
  用户实跑确认；QEMU 无真熵，跨 boot DRBG seed 相同（platform entropy=xorshift+时间），真实硬件需接 RNG。
- **既有隐患（已修，2026-09-05 第二轮）**：`pj_crypto.c` `s_key[16]="qemu-phone-cred01"`（18 字面量截断
  16）触发 initializer-string 告警；`encrypt_cred.py` `KEY=b"qemu-phone-cred01"` 注释"16 bytes"实际 17 字
  节。**复现证明现状自洽**：openssl `-K` 超长 hex 实际取前 16 字节 = `"qemu-phone-cred0"`，与 C 数组截断值
  一致，脚本输出密文与固件 `s_cred_ct` 逐字节相同（注册 200 OK 佐证）。**修复（密钥值不变、密文未重生成、
  行为 100% 不变）**：两端统一显式 16 字节 `"qemu-phone-cred0"` → C 告警消除、脚本不再依赖 openssl"忽略多
  余 hex"的隐式行为（跨版本稳健）、注释修正。复验：脚本密文仍 == 现有 `s_cred_ct`，构建零代码告警。

