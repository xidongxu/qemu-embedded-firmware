/*
 * mbedtls 裸机平台对接（ports）  --  libutils/mbedtls/ports/mbedtls_port.c
 *
 * 为 Cortex-M33 + FreeRTOS 提供 mbedtls 4.2 缺失的平台能力：
 *  1. mbedtls_ms_time()           -- MBEDTLS_PLATFORM_MS_TIME_ALT
 *       FreeRTOS tick 提供 ms 级时间（用于 TLS 证书有效期校验）。
 *  2. mbedtls_platform_get_entropy() -- MBEDTLS_PSA_DRIVER_GET_ENTROPY
 *       裸机无内置熵源；用 xorshift64* PRNG 提供伪随机熵。
 *       QEMU/TLS 验证阶段足够；生产环境应替换为硬件 RNG。
 *
 * 注意: 本文件依赖 FreeRTOS 头（FreeRTOS.h/task.h），由主构建编译；
 *       不参与 mbedtls 独立 CMake 工程。
 */
#include <stddef.h>
#include <stdint.h>

/* 先引入 ports 配置：MBEDTLS_PLATFORM_MS_TIME_ALT / MBEDTLS_PSA_DRIVER_GET_ENTROPY
 * 定义在 tf_psa_crypto_config.h（crypto 层），本文件的 #if 守卫依赖它们。
 * MBEDTLS_CONFIG_FILE / TF_PSA_CRYPTO_CONFIG_FILE 由构建传入（指向本目录）；
 * 未传入时（如 IDE 静态分析）回退到同目录相对路径。 */
#ifdef MBEDTLS_CONFIG_FILE
#include MBEDTLS_CONFIG_FILE
#else
#include "mbedtls_config.h"
#endif
#ifdef TF_PSA_CRYPTO_CONFIG_FILE
#include TF_PSA_CRYPTO_CONFIG_FILE
#else
#include "tf_psa_crypto_config.h"
#endif

#include "FreeRTOS.h"
#include "task.h"

#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "psa/crypto_values.h"
#include "psa/crypto_driver_random.h"

#if defined(MBEDTLS_PLATFORM_MS_TIME_ALT)

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    /* No RTC on this board: report a fixed epoch inside the test CA
     * certificate's validity window (2026-09-01T00:00Z) + uptime ms, so the
     * x509 notBefore/notAfter checks pass during the TLS handshake.
     * (证书有效期: 2026-08-30 ~ 2036-08-27; 板子无 RTC, 时间基准从启动算起,
     * 需落到有效期内.)  Replace with a real RTC source if available. */
    const mbedtls_ms_time_t BOOT_EPOCH_MS = 1788220800000LL;
    return BOOT_EPOCH_MS +
           (mbedtls_ms_time_t) xTaskGetTickCount() * (mbedtls_ms_time_t) portTICK_PERIOD_MS;
}

#endif /* MBEDTLS_PLATFORM_MS_TIME_ALT */

#if defined(MBEDTLS_PLATFORM_TIME_MACRO)
/* mbedtls_time() replacement: bare-metal time() returns 0 (1970), which
 * makes every x509 cert look not-yet-valid.  Return a fixed epoch inside
 * the test CA validity window + uptime seconds (see mbedtls_ms_time). */
mbedtls_time_t mbedtls_platform_time_alt(mbedtls_time_t *timer)
{
    const mbedtls_time_t BOOT_EPOCH_S = 1788220800L; /* 2026-09-01T00:00Z */
    mbedtls_time_t now = BOOT_EPOCH_S +
        (mbedtls_time_t)(xTaskGetTickCount() / (TickType_t) portTICK_PERIOD_MS / 1000);
    if (timer != NULL) {
        *timer = now;
    }
    return now;
}
#endif /* MBEDTLS_PLATFORM_TIME_MACRO */

#if defined(MBEDTLS_PSA_DRIVER_GET_ENTROPY)

int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags,
                                 size_t *estimate_bits,
                                 unsigned char *output, size_t output_size)
{
    static uint64_t s_state;

    if (flags != PSA_DRIVER_GET_ENTROPY_FLAGS_NONE) {
        return PSA_ERROR_NOT_SUPPORTED;
    }

    /* 首次用 FreeRTOS tick 做种子（QEMU 下 tick 每 ms 变化，可做弱熵）。 */
    if (s_state == 0u) {
        s_state = ((uint64_t) xTaskGetTickCount() * 0x9E3779B97F4A7C15ULL)
                ^ 0xD1B54A32D192ED03ULL;
    }

    /* xorshift64*，输出高 8 位。 */
    for (size_t i = 0; i < output_size; i++) {
        s_state ^= s_state >> 12u;
        s_state ^= s_state << 25u;
        s_state ^= s_state >> 27u;
        output[i] = (unsigned char) ((s_state * 0x2545F4914F6CDD1DULL) >> 56u);
    }

    if (estimate_bits != NULL) {
        *estimate_bits = output_size * 8u;
    }

    return 0;
}

#endif /* MBEDTLS_PSA_DRIVER_GET_ENTROPY */
