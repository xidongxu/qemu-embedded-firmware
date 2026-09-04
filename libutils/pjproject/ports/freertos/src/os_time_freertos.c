/*
 * os_time_freertos.c
 *
 * PJLIB time-of-day support for FreeRTOS. pj_gettimeofday() is derived
 * from the FreeRTOS tick counter (configTICK_RATE_HZ). No libc
 * _gettimeofday() is available in this bare-metal environment.
 */
#include <pj/os.h>
#include <pj/errno.h>

#include <FreeRTOS.h>
#include <task.h>

#define THIS_FILE   "os_time_freertos.c"

PJ_DEF(pj_status_t) pj_gettimeofday(pj_time_val *tv)
{
    TickType_t ticks;
    unsigned tick_hz;

    if (!tv)
        return PJ_EINVAL;

    ticks = xTaskGetTickCount();
    tick_hz = (unsigned)configTICK_RATE_HZ;

    tv->sec  = (long)(ticks / tick_hz);
    tv->msec = (long)((ticks % tick_hz) * (1000u / tick_hz));

    return PJ_SUCCESS;
}
