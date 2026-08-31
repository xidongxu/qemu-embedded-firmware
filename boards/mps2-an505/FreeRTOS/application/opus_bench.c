/* opus_bench.c -- standalone libopus encode/decode timing benchmark
 *
 * Purpose: isolate whether a 48k Opus encode can run in real time on the
 * 25 MHz Cortex-M33 under QEMU/TCG WITHOUT pjsua / network / FreeRTOS
 * scheduling in the picture.  The pjsua phone build showed the whole system
 * stalling (no watchdog output, no ACK) once the Opus media stream started;
 * this benchmark measures the raw per-frame cost so we can tell apart
 * "CPU is too slow" (platform limit) from "pjsua drives Opus wrong"
 * (integration bug).
 *
 * Output line:
 *   [OPUSBENCH] rate=48000 cplx=0 enc=NNNNN us/f (NN.N% of 20ms) dec=NNNNN us/f
 * A value near/above 100% of the 20 ms budget means Opus cannot keep up in
 * real time on this CPU.
 *
 * Timing uses pj_get_timestamp() (SysTick + VAL interpolation, 25 MHz wall
 * clock, NOT DWT CYCCNT which diverges from wall time under TCG).
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <opus/opus.h>
#include <pj/os.h>

#include "printf.h"
#include <FreeRTOS.h>
#include <task.h>

#define OPUSBENCH_FRAMES  50   /* frames per timed run (~1 s of media) */

static void opus_bench_one(unsigned rate, int cplx)
{
    const unsigned frame_samples = rate / 50;   /* 20 ms frame */
    pj_timestamp t0, t1, freq;
    int16_t *in = NULL, *pcm = NULL;
    unsigned char *out = NULL;
    OpusEncoder *enc = NULL;
    OpusDecoder *dec = NULL;
    unsigned i;
    uint64_t enc_us = 0, dec_us = 0;
    int enc_len = 0;
    int ret;

    in  = (int16_t *)malloc(frame_samples * sizeof(int16_t));
    pcm = (int16_t *)malloc(frame_samples * sizeof(int16_t));
    out = (unsigned char *)malloc(4000);
    if (!in || !pcm || !out) {
        printf("[OPUSBENCH] rate=%u cplx=%d malloc fail\r\n", rate, cplx);
        goto done;
    }

    /* 1 kHz sine source (matches the QEMU mic test tones). */
    for (i = 0; i < frame_samples; ++i)
        in[i] = (int16_t)(6000.0f * sinf(2.0f * 3.14159265f * 1000.0f *
                                         (float)i / (float)rate));

    /* ---- encoder ---- */
    enc = opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &ret);
    if (!enc) {
        printf("[OPUSBENCH] rate=%u cplx=%d enc_create fail (%d)\r\n",
               rate, cplx, ret);
        goto done;
    }
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(cplx));
    /* Fully mirror pjmedia opus.c encoder config (which HardFaults inside
     * silk_resampler in the phone build): signal=VOICE, bitrate=auto,
     * dtx=0, inband_fec=1 (plc), max_bw=fullband@48k, pkt_loss=5, vbr=1. */
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(OPUS_AUTO));
    opus_encoder_ctl(enc, OPUS_SET_DTX(0));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(5));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));

    pj_get_timestamp_freq(&freq);
    pj_get_timestamp(&t0);
    for (i = 0; i < OPUSBENCH_FRAMES; ++i) {
        int n = opus_encode(enc, in, (int)frame_samples, out, 4000);
        if (n < 0) {
            printf("[OPUSBENCH] rate=%u cplx=%d encode err %d\r\n",
                   rate, cplx, n);
            break;
        }
        enc_len = n;
    }
    pj_get_timestamp(&t1);
    enc_us = (t1.u64 - t0.u64) * 1000000ULL / freq.u64 / OPUSBENCH_FRAMES;
    opus_encoder_destroy(enc);
    enc = NULL;

    /* ---- decoder (decode the last real encoded packet) ---- */
    dec = opus_decoder_create(rate, 1, &ret);
    if (!dec) {
        printf("[OPUSBENCH] rate=%u cplx=%d dec_create fail (%d)\r\n",
               rate, cplx, ret);
        goto done;
    }
    pj_get_timestamp(&t0);
    for (i = 0; i < OPUSBENCH_FRAMES; ++i)
        (void)opus_decode(dec, out, enc_len, pcm, (int)frame_samples, 0);
    pj_get_timestamp(&t1);
    dec_us = (t1.u64 - t0.u64) * 1000000ULL / freq.u64 / OPUSBENCH_FRAMES;
    opus_decoder_destroy(dec);
    dec = NULL;

    printf("[OPUSBENCH] rate=%u cplx=%d enc=%llu us/f (%.1f%% of 20ms) "
           "dec=%llu us/f\r\n",
           rate, cplx,
           (unsigned long long)enc_us,
           100.0 * (double)enc_us / 20000.0,
           (unsigned long long)dec_us);

done:
    if (enc) opus_encoder_destroy(enc);
    if (dec) opus_decoder_destroy(dec);
    if (in)  free(in);
    if (pcm) free(pcm);
    if (out) free(out);
}

static void opus_bench_task(void *arg)
{
    static const unsigned rates[] = {16000, 24000, 48000};
    static const int cplx[] = {0, 1, 5};
    unsigned ri, ci;

    (void)arg;
    printf("\r\n=== OPUSBENCH: standalone libopus timing "
           "(20 ms/frame budget) ===\r\n");
    for (ri = 0; ri < (sizeof(rates) / sizeof(rates[0])); ++ri)
        for (ci = 0; ci < (sizeof(cplx) / sizeof(cplx[0])); ++ci)
            opus_bench_one(rates[ri], cplx[ci]);
    printf("[OPUSBENCH] done\r\n");
    vTaskDelete(NULL);
}

void opus_bench_start(void)
{
    xTaskCreate(opus_bench_task, "opusbench", 8192, NULL, 3U, NULL);
}
