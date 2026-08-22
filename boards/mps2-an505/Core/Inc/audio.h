/*
 * QEMU mpsx-simple-audio device (mps2-an505)
 * -------------------------------------------
 * MMIO base 0x51002000, NVIC IRQ 49.
 * See hw/audio/mpsx_simple_audio.c and hw/arm/mps2-tz.c.
 *
 * The device streams PCM data out of a guest RAM buffer (physical address),
 * configured via MMIO.  When one full buffer round has been consumed it sets
 * STATUS.DONE (and raises IRQ 49 if INT_EN.DONE is set) and then keeps
 * looping the same buffer, so playback is continuous until CTRL.ENABLE is
 * cleared.
 *
 * NOTE on IRQ 49: the FreeRTOS startup_ARMCM33.s wires slot 49 to
 * Interrupt49_Handler; call audio_irq_enable() to use the interrupt path.
 * BareMetal/threadx startups still reserve slots 49..480 as zeros, so those
 * projects must stay on the interrupt-free looping path.  The built-in test
 * (audio_test) plays via the hardware loop and works either way.
 *
 * QEMU command line (host-side) for a headless wav capture:
 *   -audiodev wav,path=out.wav,id=audio0 -machine mps2-an505,audiodev=audio0
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define AUDIO_BASE              (0x51002000UL)

#define AUDIO_CTRL_ADDR         (AUDIO_BASE + 0x00)
#define AUDIO_STATUS_ADDR       (AUDIO_BASE + 0x04)
#define AUDIO_FORMAT_ADDR       (AUDIO_BASE + 0x08)
#define AUDIO_BUF_ADDR_ADDR     (AUDIO_BASE + 0x0C)
#define AUDIO_BUF_LEN_ADDR      (AUDIO_BASE + 0x10)
#define AUDIO_SAMPLE_RATE_ADDR  (AUDIO_BASE + 0x14)
#define AUDIO_PLAY_POS_ADDR     (AUDIO_BASE + 0x18)
#define AUDIO_INT_EN_ADDR       (AUDIO_BASE + 0x1C)
#define AUDIO_INT_STATUS_ADDR   (AUDIO_BASE + 0x20)

#define REG32(addr)             (*(volatile uint32_t *)(addr))

#define AUDIO_CTRL              REG32(AUDIO_CTRL_ADDR)
#define AUDIO_STATUS            REG32(AUDIO_STATUS_ADDR)
#define AUDIO_FORMAT            REG32(AUDIO_FORMAT_ADDR)
#define AUDIO_BUF_ADDR          REG32(AUDIO_BUF_ADDR_ADDR)
#define AUDIO_BUF_LEN           REG32(AUDIO_BUF_LEN_ADDR)
#define AUDIO_SAMPLE_RATE       REG32(AUDIO_SAMPLE_RATE_ADDR)
#define AUDIO_PLAY_POS          REG32(AUDIO_PLAY_POS_ADDR)
#define AUDIO_INT_EN            REG32(AUDIO_INT_EN_ADDR)
#define AUDIO_INT_STATUS        REG32(AUDIO_INT_STATUS_ADDR)

/* Control register */
#define AUDIO_CTRL_ENABLE       (1 << 0)
#define AUDIO_CTRL_RESET        (1 << 1)
#define AUDIO_CTRL_UPDATE       (1 << 2)

/* Status register */
#define AUDIO_STATUS_BUSY       (1 << 0)
#define AUDIO_STATUS_DONE       (1 << 1)
#define AUDIO_STATUS_UNDERRUN   (1 << 2)

/* Format register: bits[1:0] sample width, bit2 stereo */
#define AUDIO_FORMAT_U8         (0)
#define AUDIO_FORMAT_S16        (1)
#define AUDIO_FORMAT_STEREO     (1 << 2)

/* Interrupt status/enable register (bit aligned between the two) */
#define AUDIO_INT_DONE          (1 << 0)

/* NVIC IRQ number (mps2-tz.c: sysbus_connect_irq(sbd, 0, get_sse_irq_in(mms, 49))) */
#define AUDIO_IRQ               (49)

/* Driver defaults */
#define AUDIO_DEFAULT_RATE      (8000)
#define AUDIO_PCM_SIZE          (32768)

/* reset device + print info */
void audio_init(void);
/* enable DONE interrupt (NVIC IRQ 49); only on targets with slot 49 wired */
void audio_irq_enable(void);
/* start looping playback */
void audio_play(const void *pcm, uint32_t len, uint32_t rate, uint32_t fmt);
/* disable playback */
void audio_stop(void);
/* restart from buffer start (clears DONE) */
void audio_update(void);
uint32_t audio_status(void);
uint32_t audio_play_pos(void);
/* wait one full buffer round; crude loop-bound timeout */
bool audio_wait_done(uint32_t timeout);
/* play a single looping sine tone */
void audio_tone(uint32_t freq_hz, uint32_t dur_ms, uint32_t vol);
/* render + play an ascending arpeggio (loops) */
void audio_test(void);

#endif /* AUDIO_H */
