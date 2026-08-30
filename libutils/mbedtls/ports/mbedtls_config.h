/*
 * mbedtls 配置（ports 覆盖）  --  libutils/mbedtls/ports/
 *
 * 通过 MBEDTLS_CONFIG_FILE 宏被 include，替代默认配置
 * （include/mbedtls/mbedtls_config.h）。
 *
 * 裁剪说明:
 *   （暂空）先引入默认配置，后续按 SIPS TLS 需求裁剪。
 *   注意: crypto 层（bignum/aes 等）配置在 TF-PSA-Crypto 的
 *   psa/crypto_config.h，见 tf_psa_crypto_config.h。
 */
#ifndef MBEDTLS_PORTS_CONFIG_H
#define MBEDTLS_PORTS_CONFIG_H

/* 引入默认 mbedtls 配置 */
#include <mbedtls/mbedtls_config.h>

/* --- ports 覆盖 --- */

/* 裸机无 socket：pjproject 用自己的网络栈（lwIP），
   关闭 mbedtls 自带 net_sockets（否则 include <sys/socket.h> 失败）。 */
#undef MBEDTLS_NET_C

/* 裸机无 OS 定时器：mbedtls_timing 依赖 gettimeofday 等，
   关闭（pj 侧用自己的超时管理）。 */
#undef MBEDTLS_TIMING_C

/* 裸机 time() 返回 0：把 mbedtls_time 重定向到自定义函数。
   (MBEDTLS_PLATFORM_TIME_MACRO 按 check_config 强制配置在
   psa/crypto_config.h，见 tf_psa_crypto_config.h；实现在 mbedtls_port.c) */

#endif /* MBEDTLS_PORTS_CONFIG_H */
