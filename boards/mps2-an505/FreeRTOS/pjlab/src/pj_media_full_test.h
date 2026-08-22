#ifndef PJ_MEDIA_FULL_TEST_H
#define PJ_MEDIA_FULL_TEST_H

/* Full-pjmedia framework self-test (stage 14).  Exercises the real pjmedia
 * media stack now linked in: endpoint, codec manager + built-in G.711,
 * event manager, RTCP session.  Returns 0 on success. */
int pj_media_full_test_run(void);

#endif /* PJ_MEDIA_FULL_TEST_H */
