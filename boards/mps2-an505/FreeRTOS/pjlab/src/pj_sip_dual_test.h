#ifndef PJ_SIP_DUAL_TEST_H
#define PJ_SIP_DUAL_TEST_H

/* Dual QEMU inter-instance call test (stage 11).
 *
 * Two patched QEMU instances (mps2-an505), each on its own user-mode
 * (slirp) network with IP 10.0.2.15, talk to each other THROUGH the host
 * gateway 10.0.2.2 using UDP hostfwd port forwards (see pj_sip_dual_test.c
 * header comment for the full address map).
 *
 * The build defines exactly one of:
 *   PJ_DUAL_ROLE_CALLER  - this instance is the UAC that dials out
 *   PJ_DUAL_ROLE_CALLEE  - this instance is the UAS that answers
 *
 * main.c runs this test instead of the loopback suite when a dual role is
 * selected at configure time (-DPJ_DUAL_ROLE=caller|callee).
 */
int pj_sip_dual_test_run(void);

#endif /* PJ_SIP_DUAL_TEST_H */
