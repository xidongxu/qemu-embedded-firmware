/*
 * pj_sip_test.c - PJSIP REGISTER loopback self-test (stage 3).
 *
 * Exercises the PJSIP stack on top of the FreeRTOS + lwIP port:
 *   - endpoint / pool factory
 *   - UDP transport bound to the board's own IP:15060
 *   - pjsip_regc (REGISTER client, from pjsip-ua)
 *   - a small module that auto-responds "200 OK" to the incoming REGISTER
 *   - full transaction round trip (client tx -> server tx -> 200 -> callback)
 *
 * The REGISTER is sent to the board's own IP:port (UDP loopback), so no
 * external server is needed.
 */

#include <stdio.h>
#include <string.h>
#include "printf.h"
#include "FreeRTOS.h"
#include "task.h"
#include "pj_sip_test.h"

#include <pj/pool.h>
#include <pj/os.h>
#include <pj/log.h>
#include <pj/string.h>
#include <pj/sock.h>
#include <pj/addr_resolv.h>
#include <pjsip.h>
#include <pjsip-ua/sip_regc.h>

#define SIP_TEST_PORT   15060
#define MAX_LOOP        80          /* 80 x 100ms = 8s max wait */

static pj_caching_pool   g_cp;
static pjsip_endpoint   *g_endpt;
static pjsip_transport  *g_tp;
static pjsip_regc       *g_regc;

static volatile int      g_reg_done;
static pj_status_t       g_reg_status;
static int               g_reg_code;

static char g_srv_url[96];
static char g_from_url[96];
static char g_to_url[96];

/* ------------------------------------------------------------------ */
/* Auto-responder module: reply "200 OK" to any REGISTER we receive.   */
/* ------------------------------------------------------------------ */
static pjsip_module mod_siptest;

static pj_status_t on_rx_request(pjsip_rx_data *rdata)
{
    if (pjsip_method_cmp(&rdata->msg_info.msg->line.req.method,
                         &pjsip_register_method) == 0) {
        pj_status_t rc;

        /* Create the server transaction, the 200 response, and send it
         * statefully in one call. */
        rc = pjsip_endpt_respond(g_endpt, &mod_siptest, rdata,
                                 200, NULL, NULL, NULL, NULL);
        PJ_LOG(4, ("pj_sip_test", "auto-responder: REGISTER -> 200 OK (rc=%d)",
                   rc));
        return PJ_TRUE;
    }
    return PJ_FALSE;
}

static pjsip_module mod_siptest =
{
    NULL, NULL,                          /* prev, next */
    {(char*)"mod-siptest", 12},          /* name */
    -1,                                  /* id */
    PJSIP_MOD_PRIORITY_APPLICATION,      /* priority (>=0, or the RX
                                            dispatcher skips it) */
    NULL,                                /* load */
    NULL,                                /* start */
    NULL,                                /* stop */
    NULL,                                /* unload */
    &on_rx_request,                      /* on_rx_request */
    NULL,                                /* on_rx_response */
    NULL,                                /* on_tx_request */
    NULL,                                /* on_tx_response */
    NULL                                 /* on_tsx_state */
};

/* ------------------------------------------------------------------ */
/* regc callback                                                       */
/* ------------------------------------------------------------------ */
static void regc_cb(struct pjsip_regc_cbparam *param)
{
    g_reg_code   = param->code;
    g_reg_status = param->status;
    g_reg_done   = 1;
    printf("pj_sip: regc callback: status=%d code=%d\r\n",
           param->status, param->code);
}

/* ------------------------------------------------------------------ */
/* Test entry                                                          */
/* ------------------------------------------------------------------ */
int pj_sip_test_run(void)
{
    pj_status_t    rc;
    pj_sockaddr_in local;
    pj_sockaddr    host;
    char           ipbuf[PJ_INET_ADDRSTRLEN];
    pj_str_t       srv, from, to;
    pjsip_tpselector tp_sel;
    pj_time_val    tmo;
    int            i;
    int            pass = 0;

    printf("\r\n=== PJSIP REGISTER loopback test ===\r\n");

    rc = pj_init();
    if (rc != PJ_SUCCESS) {
        printf("pj_sip: pj_init failed (%d)\r\n", rc);
        return -1;
    }

    pj_caching_pool_init(&g_cp, &pj_pool_factory_default_policy, 0);

    rc = pjsip_endpt_create(&g_cp.factory, "mps2-an505", &g_endpt);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip: endpt_create failed (%d)\r\n", rc);
        return -1;
    }

    /* Register the transaction layer (and mod_stateful_util) - required
     * for pjsip_endpt_send_request() to work. */
    rc = pjsip_tsx_layer_init_module(g_endpt);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip: tsx_layer_init failed (%d)\r\n", rc);
        return -1;
    }

    rc = pjsip_endpt_register_module(g_endpt, &mod_siptest);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip: register module failed (%d)\r\n", rc);
        return -1;
    }

    /* Get local IP (10.0.2.15 under QEMU user-net). */
    rc = pj_gethostip(pj_AF_INET(), &host);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip: gethostip failed (%d)\r\n", rc);
        return -1;
    }
    pj_sockaddr_print(&host, ipbuf, sizeof(ipbuf), 0);
    printf("pj_sip: local IP = %s\r\n", ipbuf);

    /* UDP transport bound to local IP:SIP_TEST_PORT. */
    pj_sockaddr_in_init(&local, NULL, (pj_uint16_t)SIP_TEST_PORT);
    local.sin_addr = host.ipv4.sin_addr;
    rc = pjsip_udp_transport_start(g_endpt, &local, NULL, 1, &g_tp);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip: udp transport failed (%d)\r\n", rc);
        return -1;
    }
    printf("pj_sip: UDP transport up on %s:%d\r\n", ipbuf, SIP_TEST_PORT);

    /* Build the URIs (loopback to ourselves). */
    pj_ansi_snprintf(g_srv_url, sizeof(g_srv_url), "sip:%s:%d",
                     ipbuf, SIP_TEST_PORT);
    pj_ansi_snprintf(g_from_url, sizeof(g_from_url), "sip:user@%s:%d",
                     ipbuf, SIP_TEST_PORT);
    pj_ansi_snprintf(g_to_url, sizeof(g_to_url), "sip:user@%s:%d",
                     ipbuf, SIP_TEST_PORT);
    pj_strset(&srv,  g_srv_url,  (pj_ssize_t)pj_ansi_strlen(g_srv_url));
    pj_strset(&from, g_from_url, (pj_ssize_t)pj_ansi_strlen(g_from_url));
    pj_strset(&to,   g_to_url,   (pj_ssize_t)pj_ansi_strlen(g_to_url));

    /* Create + init the REGISTER client. */
    rc = pjsip_regc_create(g_endpt, NULL, regc_cb, &g_regc);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip: regc_create failed (%d)\r\n", rc);
        return -1;
    }

    rc = pjsip_regc_init(g_regc, &srv, &from, &to, 0, NULL, 3600);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip: regc_init failed (%d)\r\n", rc);
        return -1;
    }

    /* Force the request out on our UDP transport (loopback). */
    pj_bzero(&tp_sel, sizeof(tp_sel));
    tp_sel.type = PJSIP_TPSELECTOR_TRANSPORT;
    tp_sel.u.transport = g_tp;
    rc = pjsip_regc_set_transport(g_regc, &tp_sel);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip: set_transport failed (%d)\r\n", rc);
        return -1;
    }

    g_reg_done   = 0;
    g_reg_status = PJ_EPENDING;
    g_reg_code   = 0;
    {
        pjsip_tx_data *tdata = NULL;
        rc = pjsip_regc_register(g_regc, PJ_FALSE, &tdata);
        if (rc != PJ_SUCCESS) {
            printf("pj_sip: regc_register failed (%d)\r\n", rc);
            return -1;
        }
        /* In pjsip 2.17, pjsip_regc_register() only builds the request;
         * pjsip_regc_send() actually transmits it and drives the callback. */
        rc = pjsip_regc_send(g_regc, tdata);
        if (rc != PJ_SUCCESS) {
            printf("pj_sip: regc_send failed (%d)\r\n", rc);
            return -1;
        }
    }

    /* Run the event loop until the callback fires. */
    tmo.sec = 0;
    tmo.msec = 100;
    for (i = 0; i < MAX_LOOP && !g_reg_done; ++i) {
        pjsip_endpt_handle_events(g_endpt, &tmo);
    }

    if (g_reg_done && g_reg_status == PJ_SUCCESS && g_reg_code == 200) {
        printf("pj_sip: REGISTER succeeded (code=%d)\r\n", g_reg_code);
        pass = 1;
    } else {
        printf("pj_sip: REGISTER FAILED (done=%d status=%d code=%d)\r\n",
               g_reg_done, g_reg_status, g_reg_code);
    }

    pjsip_regc_destroy(g_regc);
    pjsip_endpt_destroy(g_endpt);
    pj_caching_pool_destroy(&g_cp);
    pj_shutdown();

    printf("pj_sip: %s\r\n", pass ? "ALL PASSED" : "FAILED");
    return pass ? 0 : -1;
}
