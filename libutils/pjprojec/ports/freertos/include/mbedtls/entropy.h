/*
 * mbedtls/entropy.h 兼容层  --  libutils/pjprojec/ports/freertos/include/mbedtls/
 *
 * mbedtls 4.x 移除公开的 mbedtls_entropy_* API（熵已并入 PSA）。pjproject 的
 * ssl_sock_mbedtls.c 仍引用它们，这里提供兼容实现：
 *   - mbedtls_entropy_context / init / free：空对象；
 *   - mbedtls_entropy_func：委托 mbedtls_psa_get_random()（PSA 熵）。
 *
 * 本文件不改动上游 pjproject / mbedtls 源码。
 */
#ifndef PJ_PORTS_MBEDTLS_ENTROPY_H
#define PJ_PORTS_MBEDTLS_ENTROPY_H

#include <stddef.h>

#include "mbedtls/psa_util.h" /* mbedtls_psa_get_random */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int unused;
} mbedtls_entropy_context;

static inline void mbedtls_entropy_init(mbedtls_entropy_context *ctx)
{
    (void) ctx;
}

static inline void mbedtls_entropy_free(mbedtls_entropy_context *ctx)
{
    (void) ctx;
}

/* 熵回调：委托 PSA 随机。 */
static inline int mbedtls_entropy_func(void *data,
                                       unsigned char *output, size_t len)
{
    return mbedtls_psa_get_random(data, output, len);
}

#ifdef __cplusplus
}
#endif

#endif /* PJ_PORTS_MBEDTLS_ENTROPY_H */
