/*
 * QEMU mpsx-simple-mic device (mps2-an505) - microphone (audio-in)
 * -------------------------------------------
 * MMIO base 0x51003000, NVIC IRQ 50.
 * See hw/audio/mpsx_simple_mic.c and hw/arm/mps2-tz.c.
 *
 * The device captures PCM data and DMA-writes it into a guest RAM buffer
 * (physical address) configured via MMIO.  When one full buffer round has
 * been filled it sets STATUS.DONE (and raises IRQ 50 if INT_EN.DONE is
 * set), then keeps looping the buffer, so recording is continuous until
 * CTRL.ENABLE is cleared.
 *
 * NOTE on IRQ 50: startup_ARMCM33.s reserves vector slots 50..480 as
 * zeros, so the interrupt will NOT fire unless an Interrupt50_Handler
 * entry is added to the vector table first.  The built-in test therefore
 * uses the polling path (STATUS.DONE) and needs no interrupt.
 *
 * Capture source is chosen host-side:
 *   - WAV file:  -global mpsx-simple-mic.infile=<path.wav>
 *                (device feeds PCM at the WAV sample rate; SAMPLE_RATE
 *                 should be set to match the WAV rate, e.g. 8000 for
 *                 audio_test_8k.wav)
 *   - real mic:  -audiodev dsound,id=aud0,in.voices=1
 *                -global mps2-an505.audiodev=aud0
 */
#ifndef MIC_H
#define MIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MIC_BASE                (0x51003000UL)

#define MIC_CTRL_ADDR           (MIC_BASE + 0x00)
#define MIC_STATUS_ADDR         (MIC_BASE + 0x04)
#define MIC_FORMAT_ADDR         (MIC_BASE + 0x08)
#define MIC_BUF_ADDR_ADDR       (MIC_BASE + 0x0C)
#define MIC_BUF_LEN_ADDR        (MIC_BASE + 0x10)
#define MIC_SAMPLE_RATE_ADDR    (MIC_BASE + 0x14)
#define MIC_REC_POS_ADDR        (MIC_BASE + 0x18)
#define MIC_INT_EN_ADDR         (MIC_BASE + 0x1C)
#define MIC_INT_STATUS_ADDR     (MIC_BASE + 0x20)

#define REG32(addr)             (*(volatile uint32_t *)(addr))

#define MIC_CTRL                REG32(MIC_CTRL_ADDR)
#define MIC_STATUS              REG32(MIC_STATUS_ADDR)
#define MIC_FORMAT              REG32(MIC_FORMAT_ADDR)
#define MIC_BUF_ADDR            REG32(MIC_BUF_ADDR_ADDR)
#define MIC_BUF_LEN             REG32(MIC_BUF_LEN_ADDR)
#define MIC_SAMPLE_RATE         REG32(MIC_SAMPLE_RATE_ADDR)
#define MIC_REC_POS             REG32(MIC_REC_POS_ADDR)
#define MIC_INT_EN              REG32(MIC_INT_EN_ADDR)
#define MIC_INT_STATUS          REG32(MIC_INT_STATUS_ADDR)

/* Control register */
#define MIC_CTRL_ENABLE         (1 << 0)
#define MIC_CTRL_RESET          (1 << 1)
#define MIC_CTRL_UPDATE         (1 << 2)

/* Status register */
#define MIC_STATUS_BUSY         (1 << 0)
#define MIC_STATUS_DONE         (1 << 1)
#define MIC_STATUS_OVERRUN      (1 << 2)

/* Format register: bits[1:0] sample width, bit2 stereo */
#define MIC_FORMAT_U8           (0)
#define MIC_FORMAT_S16          (1)
#define MIC_FORMAT_STEREO       (1 << 2)

/* Interrupt status/enable register (bit aligned between the two) */
#define MIC_INT_DONE            (1 << 0)
#define MIC_INT_OVERRUN         (1 << 1)

/* NVIC IRQ number (mps2-tz.c: get_sse_irq_in(mms, 50)) */
#define MIC_IRQ                 (50)

/* Driver defaults */
#define MIC_DEFAULT_RATE        (8000)
#define MIC_PCM_SIZE            (32768)

/* reset device + print info */
void mic_init(void);
/* record one full buffer round of `len` bytes into buf; returns true on
 * success (data is then in buf).  rate/fmt must match the capture source. */
bool mic_capture(uint8_t *buf, uint32_t len, uint32_t rate, uint32_t fmt,
                 uint32_t timeout);
/* disable recording */
void mic_stop(void);
/* restart from buffer start (clears DONE) */
void mic_update(void);
uint32_t mic_status(void);
uint32_t mic_rec_pos(void);

#endif /* MIC_H */
