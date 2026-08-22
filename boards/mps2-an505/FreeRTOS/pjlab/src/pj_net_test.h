/**
 * @file pj_net_test.h
 * @brief PJLIB socket/ioqueue (lwIP) self-test entry point.
 */
#ifndef PJ_NET_TEST_H
#define PJ_NET_TEST_H

/**
 * Run the socket + ioqueue self-test. Call this from a FreeRTOS task after
 * the lwIP netif is up (i.e. after lwip_os_task_init()). Self-contained:
 * calls pj_init() itself.
 *
 * @return 0 on success, -1 on failure.
 */
int pj_net_test_run(void);

#endif /* PJ_NET_TEST_H */
