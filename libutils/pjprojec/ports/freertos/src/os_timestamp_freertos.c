/*
 * os_timestamp_freertos.c
 *
 * PJLIB high-resolution timestamp support for FreeRTOS.
 *
 * Stage-1 port: uses the FreeRTOS tick counter as the timestamp source,
 * so it is fully self-contained (no CMSIS / board headers needed).
 * Frequency == configTICK_RATE_HZ (1 kHz), giving millisecond resolution
 * which is sufficient for the pjlib core and timer tests.
 *
 * TODO(stage 2): upgrade to the DWT cycle counter for sub-millisecond
 * resolution once socket/ioqueue work is wired up.
 */
#include <pj/os.h>
#include <pj/errno.h>

#include <FreeRTOS.h>
#include <task.h>

#define THIS_FILE   "os_timestamp_freertos.c"

PJ_DEF(pj_status_t) pj_get_timestamp(pj_timestamp *ts)
{
    if (!ts)
        return PJ_EINVAL;

    ts->u32.lo = (pj_uint32_t)xTaskGetTickCount();
    ts->u32.hi = 0;

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_get_timestamp_freq(pj_timestamp *freq)
{
    if (!freq)
        return PJ_EINVAL;

    freq->u32.lo = (pj_uint32_t)configTICK_RATE_HZ;
    freq->u32.hi = 0;

    return PJ_SUCCESS;
}
