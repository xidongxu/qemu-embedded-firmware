/*
 * mic_test.c - mpsx-simple-mic recording smoke test (mps2-an505/QEMU).
 *
 * Records a segment from the QEMU microphone device and checks that the
 * captured PCM is not silent.  The capture source is selected host-side:
 *   - WAV file:  -global mpsx-simple-mic.infile=testcase/audio_test_8k.wav
 *   - real mic:  -audiodev dsound,id=aud0,in.voices=1
 *                -global mps2-an505.audiodev=aud0
 * (SAMPLE_RATE below must match the chosen source, e.g. 8000 for the 8k
 * file; the test analyses the signal and reports PASS/FAIL.)
 */
#include "mic_test.h"

#include <string.h>

#include "mic.h"
#include "printf.h"
/* 8000 Hz */
#define MIC_TEST_RATE       (MIC_DEFAULT_RATE)
/* 1 second of 16-bit mono */
#define MIC_TEST_LEN        (8000)
#define MIC_TEST_FMT        (MIC_FORMAT_S16)

/* "Signal present" thresholds; audio_test_8k.wav has plenty of level. */
#define MIC_TEST_PEAK_MIN   (200)
#define MIC_TEST_MEAN_MIN   (8)

static int16_t s_capture[MIC_TEST_LEN / 2];

int mic_test(void) {
    uint32_t peak = 0, zcr = 0, mean = 0;
    uint64_t abs_sum = 0;
    int32_t prev = 0;
    /* number of 16-bit samples */
    uint32_t n = MIC_TEST_LEN / 2;

    printf("\r\n===== mic test (mpsx-simple-mic) =====\r\n");

    /* record one full buffer round (the device fills it from the source) */
    if (!mic_capture((uint8_t *)s_capture, MIC_TEST_LEN, MIC_TEST_RATE, MIC_TEST_FMT, 10000000UL)) {
        printf("mic_test: capture FAILED\r\n");
        return -1;
    }

    /* basic signal analysis: peak, mean absolute, zero-crossing rate */
    for (uint32_t i = 0; i < n; i++) {
        int32_t v = s_capture[i];
        uint32_t a = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
        if (a > peak) {
            peak = a;
        }
        abs_sum += a;
        if (i > 0 && ((prev < 0 && v >= 0) || (prev >= 0 && v < 0))) {
            zcr++;
        }
        prev = v;
    }
    mean = (uint32_t)(abs_sum / n);

    printf("mic_test: captured %u samples (peak=%lu mean_abs=%lu zcr=%lu)\r\n",
           (unsigned)n, (unsigned long)peak, (unsigned long)mean,
           (unsigned long)zcr);

    if (peak >= MIC_TEST_PEAK_MIN && mean >= MIC_TEST_MEAN_MIN) {
        printf("mic_test: signal detected -> PASSED\r\n");
        return 0;
    }
    printf("mic_test: signal too weak -> FAILED\r\n");
    printf("mic_test:   check the capture source (infile WAV / audiodev) and\r\n");
    printf("mic_test:   that SAMPLE_RATE matches the source\r\n");
    return -1;
}
