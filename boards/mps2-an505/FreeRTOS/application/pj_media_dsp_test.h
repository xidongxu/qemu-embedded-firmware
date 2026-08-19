#ifndef PJ_MEDIA_DSP_TEST_H
#define PJ_MEDIA_DSP_TEST_H

/* DSP processing self-test (stage 16): verifies the pjmedia AEC
 * (echo cancellation, echo_suppress backend) and conference bridge
 * (multi-port mixing) that are compiled into pjmedia actually work.
 * Returns 0 on success. */
int pj_media_dsp_test_run(void);

#endif /* PJ_MEDIA_DSP_TEST_H */
