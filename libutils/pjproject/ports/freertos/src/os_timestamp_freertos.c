/*
 * os_timestamp_freertos.c
 *
 * PJLIB high-resolution timestamp support for FreeRTOS on Cortex-M33.
 *
 * The port's os_auto.h declares PJ_HAS_HIGH_RES_TIMER=1; the Stage-1
 * implementation (xTaskGetTickCount, 1 ms resolution) was a placeholder.
 *
 * Implementation: combine the FreeRTOS tick (1 ms, high part) with the
 * SysTick down-counter (SysTick->VAL) to interpolate sub-millisecond time.
 * SysTick is clocked by the CPU clock and advances with the same guest
 * virtual clock that drives the tick interrupt, so the combined timestamp
 * stays consistent with wall time and the frequency is EXACTLY
 * SystemCoreClock (25 MHz on the QEMU mps2-an505 / AN505).  This gives
 * sub-microsecond resolution with no drift.
 *
 * NOTE on DWT: a DWT->CYCCNT version was tried first but REJECTED (2026-08-20).
 * Under QEMU TCG the cycle counter advances at the emulated CPU rate, which is
 * NOT a fixed ratio of wall time, so a runtime-calibrated frequency drifts and
 * pjmedia_clock's 10 ms interval breaks (media collapsed: RTCP pkt=44, DTMF
 * missed, weak audio).  SysTick is reliable because it shares the virtual
 * clock with the tick.
 *
 * NOTE: this improves MEASUREMENT precision (RTCP RTT/jitter, jbuf delay).
 * It does NOT reduce network loss -- slirp host-side UDP drops and RTP
 * arrival bursts are host/emulator characteristics, unaffected by the MCU
 * clock.
 */
#include <pj/os.h>
#include <pj/errno.h>

#include <FreeRTOS.h>
#include <task.h>

#define THIS_FILE   "os_timestamp_freertos.c"

/* Cortex-M SysTick registers (kept CMSIS-free, like the rest of the port). */
#define PJ_SYSTICK_CTRL   (*(volatile uint32_t *)0xE000E010u)
#define PJ_SYSTICK_LOAD   (*(volatile uint32_t *)0xE000E014u)
#define PJ_SYSTICK_VAL    (*(volatile uint32_t *)0xE000E018u)

/* Provided by the board's system_ARMCM33.c: 25 MHz on QEMU mps2-an505. */
extern uint32_t SystemCoreClock;

PJ_DEF(pj_status_t) pj_get_timestamp(pj_timestamp *ts)
{
    uint32_t tick, load, val;
    uint64_t cycles;

    if (!ts)
        return PJ_EINVAL;

    /* FreeRTOS tick (1 ms) as the coarse part; SysTick->VAL gives the
     * sub-ms fraction.  load+1 cycles == 1 ms == one tick. */
    tick = xTaskGetTickCount();
    load = PJ_SYSTICK_LOAD;
    val  = PJ_SYSTICK_VAL & load;         /* down-counter: load..0 */

    /* Re-read if the tick advanced while we sampled VAL (boundary race). */
    if (tick != xTaskGetTickCount()) {
        tick = xTaskGetTickCount();
        val  = PJ_SYSTICK_VAL & load;
    }

    /* Cycles since boot = tick_ms * cycles_per_ms + (load - val). */
    cycles = (uint64_t)tick * (uint64_t)(load + 1u) + (uint64_t)(load - val);

    ts->u32.lo = (pj_uint32_t)cycles;
    ts->u32.hi = (pj_uint32_t)(cycles >> 32);

    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pj_get_timestamp_freq(pj_timestamp *freq)
{
    if (!freq)
        return PJ_EINVAL;

    freq->u32.lo = (pj_uint32_t)SystemCoreClock;
    freq->u32.hi = 0;

    return PJ_SUCCESS;
}
