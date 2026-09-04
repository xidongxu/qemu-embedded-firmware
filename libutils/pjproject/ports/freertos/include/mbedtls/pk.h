/*
 * mbedtls/pk.h 兼容包裹头  --  libutils/pjproject/ports/freertos/include/mbedtls/
 *
 * mbedtls 4.2 移除 mbedtls_pk_parse_key() 的 f_rng/p_rng 参数（7 参数 -> 5 参数）。
 * pjproject 的 ssl_sock_mbedtls.c 仍按 7 参数调用，这里用 include_next 引入真实
 * pk.h，再以宏把 mbedtls_pk_parse_key 重映射到 7 参数兼容函数。
 *
 * 本文件不改动上游 pjproject / mbedtls 源码。
 */
#ifndef PJ_PORTS_MBEDTLS_PK_H
#define PJ_PORTS_MBEDTLS_PK_H

#include_next <mbedtls/pk.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 7 参数兼容版（4.2 忽略 RNG 参数，转发到 5 参数真实函数）。 */
static inline int mbedtls_pk_parse_key_pjcompat(mbedtls_pk_context *ctx,
        const unsigned char *key, size_t keylen,
        const unsigned char *pwd, size_t pwdlen,
        int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
    (void) f_rng;
    (void) p_rng;
    return mbedtls_pk_parse_key(ctx, key, keylen, pwd, pwdlen);
}

#ifdef __cplusplus
}
#endif

/* 重映射到兼容函数（宏定义在函数体之后，函数体内调用不受影响）。 */
#define mbedtls_pk_parse_key mbedtls_pk_parse_key_pjcompat

#endif /* PJ_PORTS_MBEDTLS_PK_H */
