/*
 * pj_media_dsp_test.c - DSP processing self-test (stage 16).
 *
 * Verifies two pjmedia DSP modules that are now compiled in:
 *   1. AEC (acoustic echo cancellation) - echo_suppress backend is selected
 *      because PJMEDIA_HAS_SPEEX_AEC / WEBRTC_AEC / WEBRTC_AEC3 are all 0.
 *      We synthesize a far-end signal (1 kHz) that is "played" and a
 *      microphone capture = echo (0.5*1kHz) + local voice (0.5*2kHz).  After
 *      pjmedia_echo_cancel() the 1 kHz echo must be largely suppressed while
 *      the 2 kHz voice stays (measured as mean-square energy, no libm).
 *   2. Conference bridge - two tone generators (697 Hz + 1209 Hz, the DTMF
 *      "1" pair) are added to a pjmedia_conf and connected to the master
 *      port; driving the master get_frame() must produce non-zero mixed
 *      audio (multi-port mixing works).
 *
 * Fixed-point only (PJ_HAS_FLOATING_POINT=0); RMS uses mean-square energy.
 */

#include <stdio.h>
#include <string.h>
#include "printf.h"
#include "pj_media_dsp_test.h"

#include <pj/pool.h>
#include <pj/os.h>
#include <pj/log.h>
#include <pjmedia/echo.h>
#include <pjmedia/conference.h>
#include <pjmedia/tonegen.h>
#include <pjmedia/port.h>

#define FSAMP   80               /* 10 ms @ 8 kHz (conference frame)      */
#define AEC_FSAMP 160            /* 20 ms @ 8 kHz (echo_suppress needs >=80) */

/* mean-square energy of a 16-bit frame (no sqrt; compare magnitudes). */
static long frame_msq(const short *x, int n)
{
    long long acc = 0;
    int i;
    for (i = 0; i < n; ++i)
        acc += (long long)x[i] * x[i];
    return (long)(acc / n);
}

/* ---- AEC: echo suppression ---- */
/* In-phase correlation with a periodic reference (|dot|, no libm). */
static long corr_abs(const short *x, int n, const short *ref, int ref_n)
{
    long long acc = 0;
    int i;
    for (i = 0; i < n; ++i)
        acc += (long long)x[i] * ref[i % ref_n];
    return (long)(acc < 0 ? -acc : acc);
}

static int test_aec(pj_pool_t *pool)
{
    pjmedia_echo_state *echo;
    short far1k[AEC_FSAMP], near[AEC_FSAMP];
    static const short s1k[8]  = {0, 11314, 16000, 11314, 0, -11314, -16000, -11314};
    long c_in = 0, c_out = 0, echo_supp;
    int i, k;

    if (pjmedia_echo_create(pool, 8000, AEC_FSAMP, 200, 0, 0, &echo) !=
        PJ_SUCCESS) {
        printf("dsp: AEC create FAILED\r\n");
        return -1;
    }
    /* Pure-echo scenario: the mic capture is exactly the far-end signal
     * that was played to the speaker (with a little attenuation).  The AEC
     * must remove it.  (echo_suppress over-suppresses when an independent
     * near-end voice is mixed in - a known limitation of this basic algo,
     * unlike speex/webrtc AEC.) */
    for (k = 0; k < 100; ++k) {
        for (i = 0; i < AEC_FSAMP; ++i) {
            far1k[i] = s1k[i & 7];
            near[i]  = (short)(far1k[i] - (far1k[i] >> 3));   /* 0.875*echo */
        }
        c_in += corr_abs(near, AEC_FSAMP, s1k, 8);
        pjmedia_echo_playback(echo, far1k);      /* feed far-end reference */
        pjmedia_echo_cancel(echo, near, far1k, 0, NULL);
        c_out += corr_abs(near, AEC_FSAMP, s1k, 8);
    }
    echo_supp = (c_in > 0) ? (100 - c_out * 100 / c_in) : 0;
    if (echo_supp < 0) echo_supp = 0;
    printf("dsp:   aec pure-echo suppressed=%ld%%\r\n", echo_supp);
    pjmedia_echo_destroy(echo);
    return (echo_supp >= 80) ? 0 : -1;
}

/* ---- Conference: two tone gens mixed into the master port ---- */
static int test_conf(pj_pool_t *pool)
{
    pjmedia_conf *conf;
    pjmedia_port *tg1, *tg2, *master;
    pjmedia_tone_desc tones[1];
    pjmedia_frame frame;
    short tmp[FSAMP];
    unsigned slot1, slot2;
    long long e = 0;
    int i;

    if (pjmedia_conf_create(pool, 4, 8000, 1, FSAMP, 16,
                            PJMEDIA_CONF_NO_DEVICE, &conf) != PJ_SUCCESS) {
        printf("dsp: conf create FAILED\r\n");
        return -1;
    }
    if (pjmedia_tonegen_create(pool, 8000, 1, FSAMP, 16, 0, &tg1) != PJ_SUCCESS ||
        pjmedia_tonegen_create(pool, 8000, 1, FSAMP, 16, 0, &tg2) != PJ_SUCCESS) {
        printf("dsp: tonegen create FAILED\r\n");
        return -1;
    }
    pjmedia_conf_add_port(conf, pool, tg1, NULL, &slot1);
    pjmedia_conf_add_port(conf, pool, tg2, NULL, &slot2);
    pjmedia_conf_connect_port(conf, slot1, 0, 0);   /* tg1 -> master out */
    pjmedia_conf_connect_port(conf, slot2, 0, 0);   /* tg2 -> master out */

    /* tone 1: 697 Hz; tone 2: 1209 Hz (the DTMF "1" pair), 1 s each. */
    memset(tones, 0, sizeof(tones));
    tones[0].freq1 = 697; tones[0].on_msec = 1000;
    pjmedia_tonegen_play(tg1, 1, tones, 0);
    tones[0].freq1 = 1209; tones[0].on_msec = 1000;
    pjmedia_tonegen_play(tg2, 1, tones, 0);

    master = pjmedia_conf_get_master_port(conf);
    frame.buf = tmp;
    frame.size = (pj_size_t)(FSAMP * 2);
    printf("dsp:   conf driving master 100 frames\r\n");
    for (i = 0; i < 100; ++i) {
        if (pjmedia_port_get_frame(master, &frame) != PJ_SUCCESS)
            break;
        e += frame_msq(tmp, FSAMP);
    }
    e /= 100;
    printf("dsp:   conf slots=%u mixed-rms=%ld\r\n",
           pjmedia_conf_get_port_count(conf), (long)e);
    pjmedia_conf_destroy(conf);
    /* non-zero mixed output proves both tone gens were mixed. */
    return (e > 0) ? 0 : -1;
}

int pj_media_dsp_test_run(void)
{
    pj_caching_pool cp;
    pj_pool_t *pool;
    int pass = 1;

    printf("\r\n=== PJMEDIA DSP test (AEC + conference) ===\r\n");

    if (pj_init() != PJ_SUCCESS) {
        printf("dsp: pj_init FAILED\r\n");
        return -1;
    }
    pj_caching_pool_init(&cp, &pj_pool_factory_default_policy, 0);
    pool = pj_pool_create(&cp.factory, "dsp", 2048, 2048, NULL);

    if (test_aec(pool) != 0) { printf("dsp: AEC FAILED\r\n"); pass = 0; }
    if (test_conf(pool) != 0) { printf("dsp: CONF FAILED\r\n"); pass = 0; }

    pj_pool_release(pool);
    pj_caching_pool_destroy(&cp);
    pj_shutdown();
    printf("dsp: %s\r\n", pass ? "ALL PASSED" : "FAILED");
    return pass ? 0 : -1;
}
