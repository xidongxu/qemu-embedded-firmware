/*
 * mic.c - driver for the QEMU mpsx-simple-mic device (mps2-an505)
 *
 * The QEMU device captures PCM data and DMA-writes it into a guest RAM
 * buffer configured via MMIO.  After one full buffer round it sets
 * STATUS.DONE and (if INT_EN.DONE is set) raises NVIC IRQ 50.  This
 * driver waits for the DONE interrupt (Interrupt50_Handler sets a flag),
 * falling back to polling STATUS.DONE if the interrupt is not wired up.
 */

#include "ARMCM33_DSP_FP.h"
#include "mic.h"

/* incremented by Interrupt50_Handler on each DONE interrupt */
static volatile uint32_t s_mic_irq_done = 0;

/*
 * Weak hook invoked from Interrupt50_Handler after clearing DONE.  The
 * mpsx pjmedia-audiodev backend (application/mpsx_dev.c) overrides it to
 * signal its capture task; other projects keep the weak no-op.
 */
__attribute__((weak)) void mic_done_hook(void) {}

void mic_init(void) {
    /* device-side reset */
    MIC_CTRL = MIC_CTRL_RESET;
    /* clear any pending interrupt status */
    MIC_INT_STATUS = MIC_INT_DONE | MIC_INT_OVERRUN;
    /* enable the DONE interrupt on the device side... */
    MIC_INT_EN = MIC_INT_DONE;
    /* ...and route NVIC IRQ 50 to Interrupt50_Handler */
    NVIC_ClearPendingIRQ((IRQn_Type)MIC_IRQ);
    NVIC_EnableIRQ((IRQn_Type)MIC_IRQ);
    printf("mic: reset done, status=0x%08lx (IRQ %d enabled)\n",
           (unsigned long)MIC_STATUS, (int)MIC_IRQ);
}

/*
 * Record one full buffer round of `len` bytes into `buf`.  The device is
 * configured with the format/rate/buffer and enabled, then we wait for
 * STATUS.DONE (one full buffer has been filled) with a loop timeout.
 * Returns true on success, in which case `len` bytes are now in `buf`.
 */
bool mic_capture(uint8_t *buf, uint32_t len, uint32_t rate, uint32_t fmt,
                 uint32_t timeout) {
    uint32_t t = 0;
    uint32_t irq_before = 0;
    if (buf == NULL || len == 0) {
        printf("mic: invalid capture args (buf=%p len=%lu)\n",
               (void *)buf, (unsigned long)len);
        return false;
    }
    mic_stop();
    __DSB();
    MIC_FORMAT = fmt;
    MIC_SAMPLE_RATE = rate;
    MIC_BUF_ADDR = (uint32_t)(uintptr_t)buf;
    MIC_BUF_LEN = len;
    MIC_INT_STATUS = MIC_INT_DONE | MIC_INT_OVERRUN;
    /* baseline interrupt counter, then start capture from buffer start */
    irq_before = s_mic_irq_done;
    __DSB();
    MIC_CTRL = MIC_CTRL_ENABLE | MIC_CTRL_UPDATE;
    /* wait one full buffer round: primary = DONE interrupt, fallback = STATUS */
    while (s_mic_irq_done == irq_before) {
        if (MIC_STATUS & MIC_STATUS_DONE) {
            break;   /* polled path (interrupt not wired up) */
        }
        if (++t >= timeout) {
            printf("mic: capture timeout (status=0x%08lx)\n",
                   (unsigned long)MIC_STATUS);
            return false;
        }
    }
    printf("mic: captured %lu bytes @ %lu Hz fmt=0x%lx "
           "(buf=0x%08lx, rec_pos=%lu, irq_done=%lu)\n",
           (unsigned long)len, (unsigned long)rate, (unsigned long)fmt,
           (unsigned long)(uintptr_t)buf, (unsigned long)MIC_REC_POS,
           (unsigned long)s_mic_irq_done);
    return true;
}

void mic_stop(void) {
    MIC_CTRL = 0;
}

void mic_update(void) {
    MIC_CTRL = MIC_CTRL_ENABLE | MIC_CTRL_UPDATE;
}

uint32_t mic_status(void) {
    return MIC_STATUS;
}

uint32_t mic_rec_pos(void) {
    return MIC_REC_POS;
}

/*
 * IRQ handler for the mic device (NVIC IRQ 50).
 * Requires the startup vector table to install Interrupt50_Handler
 * (already done in startup_ARMCM33.s).  Only touches the flag + MMIO,
 * so it is safe to call from an interrupt context.
 */
void Interrupt50_Handler(void) {
    uint32_t st = MIC_INT_STATUS;
    if (st & MIC_INT_DONE) {
        /* write-1-to-clear */
        MIC_INT_STATUS = MIC_INT_DONE;
        s_mic_irq_done++;
        mic_done_hook();
    }
    if (st & MIC_INT_OVERRUN) {
        MIC_INT_STATUS = MIC_INT_OVERRUN;
    }
}
