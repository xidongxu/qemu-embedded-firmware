/**
 * @file pj_test.h
 * @brief PJLIB FreeRTOS port self-test entry point.
 */
#ifndef PJ_TEST_H
#define PJ_TEST_H

/**
 * Run the PJLIB self-test (thread/mutex/sem/atomic/timer/pool/time).
 * Call this from a FreeRTOS task (the scheduler must already be running).
 * Prints PASS/FAIL lines over the UART.
 *
 * @return 0 on success, -1 if any sub-test failed.
 */
int pj_test_run(void);

#endif /* PJ_TEST_H */
