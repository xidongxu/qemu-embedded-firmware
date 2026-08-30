/*
 * TF-PSA-Crypto 配置（ports 覆盖）  --  libutils/mbedtls/ports/
 *
 * 通过 TF_PSA_CRYPTO_CONFIG_FILE 宏被 include，替代默认配置
 * （tf-psa-crypto/include/psa/crypto_config.h）。
 *
 * 裁剪说明:
 *   MBEDTLS_HAVE_ASM  -- 关闭内联汇编。
 *   Cortex-M33 (arm-none-eabi-gcc) 下 bignum_core.c 的汇编
 *   ('asm' operand has impossible constraints) 无法编译；
 *   且 tf_psa_crypto_check_config.h 禁止
 *   MBEDTLS_HAVE_INT32/INT64 与 MBEDTLS_HAVE_ASM 同时定义。
 */
#ifndef TF_PSA_CRYPTO_PORTS_CONFIG_H
#define TF_PSA_CRYPTO_PORTS_CONFIG_H

/* 引入默认配置 */
#include <psa/crypto_config.h>

/* --- ports 覆盖 --- */

/* 关闭内联汇编（Cortex-M33 下不可用，改纯 C 实现） */
#undef MBEDTLS_HAVE_ASM

/* 自定义 ms 级时间源（FreeRTOS tick），替代 OS 时间。
   注意: check_config 强制此宏在 psa/crypto_config.h（即本文件）配置。 */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* 自定义熵源（裸机无 /dev/urandom），替代内置熵。
   实现见 ports/mbedtls_port.c 的 mbedtls_platform_get_entropy()。 */
#undef MBEDTLS_PSA_BUILTIN_GET_ENTROPY
#define MBEDTLS_PSA_DRIVER_GET_ENTROPY

/* 裸机无文件系统：关闭 MBEDTLS_FS_IO。
   否则 mbedtls_x509_crt_parse_file/path 会用到 DIR/opendir（newlib 裸机
   dirent.h 未定义 DIR 类型）。证书一律从内存加载（mbedtls_x509_crt_parse）。 */
#undef MBEDTLS_FS_IO

/* PSA 持久化密钥存储与 ITS 文件存储都依赖文件系统/存储实现：
   - MBEDTLS_PSA_CRYPTO_STORAGE_C 关掉后 psa_crypto_storage.c 整体不编译
     （否则其 Native ITS 分支会 include 缺失的 psa/error.h）。
   - MBEDTLS_PSA_ITS_FILE_C 依赖 FS_IO，需一并关闭以免 check_config 报错。
   本工程用 mbedtls 传统 API（TLS/X509），无需 PSA 持久化存储。 */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

/* 裸机 time() 返回 0(1970)，x509 有效期检查会把证书判为未生效(FUTURE)。
   用 MBEDTLS_PLATFORM_TIME_MACRO 把 mbedtls_time 重定向到自定义函数
   mbedtls_platform_time_alt()（实现见 mbedtls_port.c），返回 2026-09-01
   基准 + uptime 秒（落在测试 CA 有效期内）。
   注意: check_config 强制此宏配置在 psa/crypto_config.h（即本文件）。 */
#include <time.h>
time_t mbedtls_platform_time_alt(time_t *timer);
#define MBEDTLS_PLATFORM_TIME_MACRO mbedtls_platform_time_alt

#endif /* TF_PSA_CRYPTO_PORTS_CONFIG_H */
