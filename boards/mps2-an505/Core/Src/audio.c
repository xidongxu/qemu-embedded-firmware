/*
 * audio.c - driver for the QEMU mpsx-simple-audio device (mps2-an505)
 *
 * The QEMU device streams PCM data out of a guest RAM buffer configured via
 * MMIO.  After one full buffer round it sets STATUS.DONE and (if INT_EN.DONE
 * is set) raises NVIC IRQ 49, then keeps looping the buffer.  The FreeRTOS
 * startup wires slot 49 to Interrupt49_Handler; call audio_irq_enable() to
 * use the interrupt path.  BareMetal/threadx leave slot 49 as zero, so those
 * projects stay on the interrupt-free looping path.
 */

#include "ARMCM33_DSP_FP.h"
#include "audio.h"

/* PCM buffer played by the device; the physical address is handed to BUF_ADDR. */
static uint8_t s_pcm[AUDIO_PCM_SIZE] __attribute__((aligned(64)));

/* incremented by Interrupt49_Handler on each DONE interrupt */
static volatile uint32_t s_audio_irq_done = 0;

/*
 * 64-point sine lookup table, values are round(127 * sin(2*pi*k/64)).
 * Using a fixed-point phase accumulator + this LUT keeps the driver free of
 * libm/float dependencies (newlib-nano + hard-float can't link sinf).
 */
#define SINE_LUT_SIZE   (64)
/* LUT index 0..63 (6 bits) + 16-bit frac */
#define SINE_PHASE_BITS (22)
static const int8_t sine_lut[SINE_LUT_SIZE] = {
     0,  12,  25,  37,  49,  60,  71,  81,
    90,  98, 106, 112, 117, 122, 125, 126,
   127, 126, 125, 122, 117, 112, 106,  98,
    90,  81,  71,  60,  49,  37,  25,  12,
     0, -12, -25, -37, -49, -60, -71, -81,
   -90, -98,-106,-112,-117,-122,-125,-126,
  -127,-126,-125,-122,-117,-112,-106, -98,
   -90, -81, -71, -60, -49, -37, -25, -12,
};

/*
 * Render `dur_ms` of a sine tone at `freq_hz` into buf (U8 mono, silence=0x80).
 * `phase` is a 32-bit fixed-point phase accumulator (LUT cycle = 1<<SINE_PHASE_BITS)
 * kept across calls so consecutive notes stay phase-continuous (no clicks).
 */
static uint32_t audio_render_tone(uint8_t *buf, uint32_t max_bytes, uint32_t rate,
                                  uint32_t freq_hz, uint32_t dur_ms, uint32_t vol,
                                  uint32_t *phase) {
    uint32_t n = (uint32_t)(((uint64_t)rate * dur_ms) / 1000UL);
    uint32_t step;
    if (n > max_bytes) {
        n = max_bytes;
    }
    step = (uint32_t)(((uint64_t)freq_hz << SINE_PHASE_BITS) / rate);
    for (uint32_t i = 0; i < n; i++) {
        /* 0..63 */
        uint32_t idx = *phase >> 16;
        uint32_t frac = *phase & 0xFFFF;
        int32_t y0 = sine_lut[idx];
        int32_t y1 = sine_lut[(idx + 1) & (SINE_LUT_SIZE - 1)];
        int32_t y = y0 + (((y1 - y0) * (int32_t)frac) >> 16);
        buf[i] = (uint8_t)(128 + (y * (int32_t)vol) / 127);
        /* wraps modulo 2^32 == a multiple of the LUT cycle */
        *phase += step;
    }
    return n;
}

static uint32_t audio_render_silence(uint8_t *buf, uint32_t max_bytes, uint32_t rate, uint32_t dur_ms) {
    uint32_t n = (uint32_t)(((uint64_t)rate * dur_ms) / 1000UL);
    if (n > max_bytes) {
        n = max_bytes;
    }
    /* U8 silence is mid-level 0x80 */
    memset(buf, 0x80, n);
    return n;
}

void audio_init(void) {
    /* device-side reset */
    AUDIO_CTRL = AUDIO_CTRL_RESET;
    /* clear any pending interrupt status */
    AUDIO_INT_STATUS = AUDIO_INT_DONE;
    printf("audio: pcm buffer @ %p (%lu bytes)\n", (void *)s_pcm, (unsigned long)AUDIO_PCM_SIZE);
    printf("audio: reset done, status=0x%08lx\n", (unsigned long)AUDIO_STATUS);
}

/*
 * Enable the DONE interrupt (NVIC IRQ 49).  Only call this on targets whose
 * startup vector table wires slot 49 to Interrupt49_Handler (the FreeRTOS gcc
 * startup does; BareMetal/threadx leave it zero and must NOT call this).
 */
void audio_irq_enable(void) {
    AUDIO_INT_STATUS = AUDIO_INT_DONE;      /* clear any pending status */
    AUDIO_INT_EN = AUDIO_INT_DONE;          /* device-side DONE interrupt */
    NVIC_ClearPendingIRQ((IRQn_Type)AUDIO_IRQ);
    NVIC_EnableIRQ((IRQn_Type)AUDIO_IRQ);
    printf("audio: IRQ %d enabled\n", (int)AUDIO_IRQ);
}

void audio_play(const void *pcm, uint32_t len, uint32_t rate, uint32_t fmt) {
    if (pcm == NULL || len == 0) {
        printf("audio: invalid play args (pcm=%p len=%lu)\n", pcm, (unsigned long)len);
        return;
    }
    audio_stop();
    __DSB();
    AUDIO_FORMAT = fmt;
    AUDIO_SAMPLE_RATE = rate;
    AUDIO_BUF_ADDR = (uint32_t)pcm;
    AUDIO_BUF_LEN = len;
    AUDIO_INT_STATUS = AUDIO_INT_DONE;
    __DSB();
    AUDIO_CTRL = AUDIO_CTRL_ENABLE | AUDIO_CTRL_UPDATE;
    printf("audio: playing %lu bytes @ %lu Hz fmt=0x%lx (buf=0x%08lx)\n",
           (unsigned long)len, (unsigned long)rate, (unsigned long)fmt,
           (unsigned long)(uintptr_t)pcm);
}

void audio_stop(void) {
    AUDIO_CTRL = 0;
}

void audio_update(void) {
    AUDIO_CTRL = AUDIO_CTRL_ENABLE | AUDIO_CTRL_UPDATE;
}

uint32_t audio_status(void) {
    return AUDIO_STATUS;
}

uint32_t audio_play_pos(void) {
    return AUDIO_PLAY_POS;
}

bool audio_wait_done(uint32_t timeout) {
    uint32_t t = 0;
    uint32_t irq_before = s_audio_irq_done;
    /* one full buffer round: primary = DONE interrupt, fallback = STATUS */
    while (s_audio_irq_done == irq_before) {
        if (AUDIO_STATUS & AUDIO_STATUS_DONE) {
            break;   /* polled path (interrupt not enabled) */
        }
        if (++t >= timeout) {
            return false;
        }
    }
    return true;
}

void audio_tone(uint32_t freq_hz, uint32_t dur_ms, uint32_t vol) {
    uint32_t phase = 0;
    uint32_t n = audio_render_tone(s_pcm, AUDIO_PCM_SIZE, AUDIO_DEFAULT_RATE, freq_hz, dur_ms, vol, &phase);
    printf("audio: tone %lu Hz, %lu ms\n", (unsigned long)freq_hz, (unsigned long)dur_ms);
    audio_play(s_pcm, n, AUDIO_DEFAULT_RATE, AUDIO_FORMAT_U8);
}

void audio_test(void) {
    /* Ascending C-major arpeggio, then a sustained tonic so the loop
     * boundary is easy to hear.  The whole melody fits in s_pcm and is
     * played continuously by the hardware looping. */
    static const float notes_hz[] = {
        261.63f,  /* C4 */
        329.63f,  /* E4 */
        392.00f,  /* G4 */
        523.25f,  /* C5 */
        659.25f,  /* E5 */
        783.99f,  /* G5 */
        1046.50f, /* C6 */
    };
    const uint32_t rate     = AUDIO_DEFAULT_RATE;
    const uint32_t note_ms  = 300;
    const uint32_t gap_ms   = 60;
    const uint32_t final_ms = 1000;
    const uint32_t vol      = 90;
    uint32_t phase = 0;
    uint32_t pos = 0;
    uint32_t i;

    for (i = 0; i < sizeof(notes_hz) / sizeof(notes_hz[0]); i++) {
        pos += audio_render_tone(s_pcm + pos, AUDIO_PCM_SIZE - pos, rate, (uint32_t)notes_hz[i], note_ms, vol, &phase);
        pos += audio_render_silence(s_pcm + pos, AUDIO_PCM_SIZE - pos, rate, gap_ms);
    }
    pos += audio_render_tone(s_pcm + pos, AUDIO_PCM_SIZE - pos, rate, (uint32_t)notes_hz[0], final_ms, vol, &phase);

    if (pos == 0) {
        printf("audio: nothing rendered\n");
        return;
    }
    printf("audio: test melody %lu ms, looping...\n", (unsigned long)((uint64_t)pos * 1000UL / rate));
    audio_play(s_pcm, pos, rate, AUDIO_FORMAT_U8);
    /* wait one full buffer round: primary = DONE interrupt (IRQ 49), so the
     * handler's irq_done counter proves the interrupt really fires; targets
     * without slot 49 fall back to polling STATUS.DONE. */
    if (audio_wait_done(20000000UL)) {
        printf("audio: first buffer round done (irq_done=%lu)\n",
               (unsigned long)s_audio_irq_done);
    }
}

/*
 * IRQ handler for the audio device (NVIC IRQ 49).
 * Requires the startup vector table to install Interrupt49_Handler (wired up
 * in the FreeRTOS startup_ARMCM33.s).  Only touches the flag + MMIO, so it
 * is safe to call from an interrupt context.
 */
void Interrupt49_Handler(void) {
    uint32_t st = AUDIO_INT_STATUS;
    if (st & AUDIO_INT_DONE) {
        /* write-1-to-clear */
        AUDIO_INT_STATUS = AUDIO_INT_DONE;
        s_audio_irq_done++;
    }
}
