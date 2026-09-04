/*
 * mbedtls/ctr_drbg.h 兼容层  --  libutils/pjproject/ports/freertos/include/mbedtls/
 *
 * mbedtls 4.x 把 CTR_DRBG 变为 TF-PSA-Crypto 内部(私有)模块，公开的
 * mbedtls_ctr_drbg_* API 已移除。pjproject 的 ssl_sock_mbedtls.c 仍使用该 API，
 * 这里在 ports include 目录（优先于上游头）提供兼容实现：
 *   - seed/init/free 为空操作；
 *   - random 委托 mbedtls_psa_get_random()（4.2 公开 API，内部走 PSA 随机，
 *     熵来自 libutils/mbedtls/ports 的自定义 mbedtls_platform_get_entropy()）。
 *
 * 本文件不改动上游 pjproject / mbedtls 源码。
 */
#ifndef PJ_PORTS_MBEDTLS_CTR_DRBG_H
#define PJ_PORTS_MBEDTLS_CTR_DRBG_H

#include <stddef.h>

#include "mbedtls/psa_util.h" /* mbedtls_psa_get_random */

#ifdef __cplusplus
extern "C" {
#endif

/* 无真实内部状态（random 直接走 PSA）。 */
typedef struct {
    int unused;
} mbedtls_ctr_drbg_context;

typedef int (*mbedtls_f_rng)(void *, unsigned char *, size_t);

/* 空初始化 / 释放 */
static inline void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *ctx)
{
    (void) ctx;
}

static inline void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *ctx)
{
    (void) ctx;
}

/* 无需播种（PSA 随机自带熵源），返回成功。 */
static inline int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *ctx,
                                        int (*f_entropy)(void *, unsigned char *, size_t),
                                        void *p_entropy,
                                        const unsigned char *custom, size_t len)
{
    (void) ctx; (void) f_entropy; (void) p_entropy;
    (void) custom; (void) len;
    return 0;
}

/* RNG 回调：委托 mbedtls_psa_get_random()（p_rng 被忽略）。 */
static inline int mbedtls_ctr_drbg_random(void *p_rng,
                                          unsigned char *output, size_t len)
{
    return mbedtls_psa_get_random(p_rng, output, len);
}

#ifdef __cplusplus
}
#endif

#endif /* PJ_PORTS_MBEDTLS_CTR_DRBG_H */
