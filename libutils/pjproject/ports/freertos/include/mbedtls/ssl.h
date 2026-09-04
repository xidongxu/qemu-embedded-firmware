/*
 * mbedtls/ssl.h 兼容包裹头  --  libutils/pjproject/ports/freertos/include/mbedtls/
 *
 * mbedtls 4.2 移除了 mbedtls_ssl_conf_rng()（TLS RNG 改走 PSA）。pjproject 的
 * ssl_sock_mbedtls.c 仍调用它，这里提供空实现（RNG 已由 mbedtls 内部 PSA 处理，
 * 无需再设置）。
 *
 * 本文件不改动上游 pjproject / mbedtls 源码。
 */
#ifndef PJ_PORTS_MBEDTLS_SSL_H
#define PJ_PORTS_MBEDTLS_SSL_H

#include_next <mbedtls/ssl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 兼容 mbedtls 3.x 的 mbedtls_ssl_conf_rng（4.2 移除，RNG 走 PSA）。
 * 类型 mbedtls_f_rng / mbedtls_ssl_config 来自真实 ssl.h（或 ctr_drbg.h）。 */
#if !defined(PJ_PORTS_MBEDTLS_F_RNG_DEFINED)
typedef int (*mbedtls_f_rng)(void *, unsigned char *, size_t);
#define PJ_PORTS_MBEDTLS_F_RNG_DEFINED
#endif

static inline void mbedtls_ssl_conf_rng(mbedtls_ssl_config *conf,
                                        mbedtls_f_rng f_rng, void *p_rng)
{
    (void) conf;
    (void) f_rng;
    (void) p_rng;
}

#ifdef __cplusplus
}
#endif

#endif /* PJ_PORTS_MBEDTLS_SSL_H */
