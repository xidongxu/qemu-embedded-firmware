/*
 * pj_sip_inv_test.c - PJSIP INVITE loopback self-test (stage 4).
 *
 * Exercises the full INVITE session machinery now that pjmedia SDP
 * (sdp.c + sdp_neg.c, trimmed codec stubs) is available:
 *   - pjsip_ua layer (dialog management)
 *   - pjsip_inv INVITE session (UAC + UAS)
 *   - SDP offer/answer negotiation through pjmedia_sdp_neg
 *
 * The UAC sends an INVITE (with an audio/PCMU SDP offer) to the board's own
 * UDP transport (loopback).  A module acts as the UAS, creates a dialog +
 * invite session, and answers 200 OK with an SDP answer.  The negotiator
 * produces an active local/remote SDP, and both sessions reach
 * PJSIP_INV_STATE_CONFIRMED after the automatic ACK.
 */

#include <stdio.h>
#include <string.h>
#include "printf.h"
#include "FreeRTOS.h"
#include "task.h"
#include "pj_sip_inv_test.h"

#include <pj/pool.h>
#include <pj/os.h>
#include <pj/log.h>
#include <pj/string.h>
#include <pj/sock.h>
#include <pj/addr_resolv.h>
#include <pjsip.h>
#include <pjsip-ua/sip_dialog.h>
#include <pjsip/sip_ua_layer.h>
#include <pjsip-ua/sip_inv.h>
#include <pjmedia/sdp.h>
#include <pjmedia/sdp_neg.h>

#define SIP_INV_PORT    15062
#define MAX_LOOP        100         /* 100 x 100ms = 10s max wait */

static pj_caching_pool   g_cp;
static pjsip_endpoint   *g_endpt;
static pjsip_transport  *g_tp;
static pjsip_user_agent *g_ua;

static pjsip_dialog      *g_uac_dlg;
static pjsip_inv_session *g_uac_inv;
static pjsip_inv_session *g_uas_inv;

static volatile int       g_uac_done;
static volatile int       g_uas_done;

static pjmedia_sdp_session *g_offer_sdp;   /* UAC offer  */
static pjmedia_sdp_session *g_ans_sdp;     /* UAS answer capability */

static char g_local_ip[PJ_INET_ADDRSTRLEN];

#define SET_STR(s, lit) do { (s).ptr = (char*)(lit); \
                             (s).slen = (pj_ssize_t)(sizeof(lit)-1); } while (0)

/* ------------------------------------------------------------------ */
/* Small helper: build an audio/PCMU SDP session descriptor.           */
/* ------------------------------------------------------------------ */
static pjmedia_sdp_session *create_audio_sdp(pj_pool_t *pool,
                                             pj_uint16_t port)
{
    pjmedia_sdp_session *sess;
    pjmedia_sdp_media *m;
    pjmedia_sdp_attr *a;
    pjmedia_sdp_rtpmap rtpmap;

    sess = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_session);
    SET_STR(sess->origin.user, "mps2-an505");
    sess->origin.id = 0;
    sess->origin.version = 0;
    SET_STR(sess->origin.net_type, "IN");
    SET_STR(sess->origin.addr_type, "IP4");
    SET_STR(sess->origin.addr, g_local_ip);
    SET_STR(sess->name, "invite-test");

    sess->conn = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_conn);
    SET_STR(sess->conn->net_type, "IN");
    SET_STR(sess->conn->addr_type, "IP4");
    SET_STR(sess->conn->addr, g_local_ip);
    sess->time.start = 0;
    sess->time.stop = 0;

    sess->media_count = 1;
    sess->media[0] = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_media);
    m = sess->media[0];
    SET_STR(m->desc.media, "audio");
    m->desc.port = port;
    m->desc.port_count = 0;
    SET_STR(m->desc.transport, "RTP/AVP");
    m->desc.fmt_count = 1;
    SET_STR(m->desc.fmt[0], "0");

    pj_bzero(&rtpmap, sizeof(rtpmap));
    SET_STR(rtpmap.pt, "0");
    SET_STR(rtpmap.enc_name, "PCMU");
    rtpmap.clock_rate = 8000;
    rtpmap.param.ptr = NULL;
    rtpmap.param.slen = 0;
    if (pjmedia_sdp_rtpmap_to_attr(pool, &rtpmap, &a) == PJ_SUCCESS)
        pjmedia_sdp_media_add_attr(m, a);

    return sess;
}

/* ------------------------------------------------------------------ */
/* INVITE session state callback (global, registered with the usage).  */
/* ------------------------------------------------------------------ */
static void inv_on_state_changed(pjsip_inv_session *inv, pjsip_event *e)
{
    static const char *st[] = { "NULL", "CALLING", "INCOMING", "EARLY",
                                "CONNECTING", "CONFIRMED", "DISCONNECTED" };
    PJ_UNUSED_ARG(e);

    printf("pj_sip_inv: [%s] state -> %s (cause=%d)\\
",
           inv->role == PJSIP_ROLE_UAC ? "UAC" : "UAS",
           st[inv->state], (int)inv->cause);

    if (inv->state == PJSIP_INV_STATE_CONFIRMED) {
        if (inv == g_uac_inv) g_uac_done = 1;
        if (inv == g_uas_inv) g_uas_done = 1;
    }
}

/* ------------------------------------------------------------------ */
/* UAS module: auto-answer the initial INVITE with 200 OK + SDP.       */
/* ------------------------------------------------------------------ */
static pjsip_module mod_inv_test;

static pj_status_t uas_on_rx_request(pjsip_rx_data *rdata)
{
    if (pjsip_method_cmp(&rdata->msg_info.msg->line.req.method,
                         &pjsip_invite_method) == 0) {
        pjsip_tx_data *tdata = NULL;
        pj_status_t rc;

        rc = pjsip_dlg_create_uas(g_ua, rdata, NULL, &g_uac_dlg);
        if (rc != PJ_SUCCESS) {
            printf("pj_sip_inv: UAS dlg_create failed (%d)\\
", rc);
            return PJ_TRUE;
        }
        rc = pjsip_inv_create_uas(g_uac_dlg, rdata, g_ans_sdp, 0, &g_uas_inv);
        if (rc != PJ_SUCCESS) {
            printf("pj_sip_inv: UAS inv_create failed (%d)\\
", rc);
            return PJ_TRUE;
        }
        rc = pjsip_inv_answer(g_uas_inv, 200, NULL, NULL, &tdata);
        if (rc == PJ_SUCCESS)
            rc = pjsip_inv_send_msg(g_uas_inv, tdata);
        printf("pj_sip_inv: UAS answered 200 OK (rc=%d)\\
", rc);
        return PJ_TRUE;
    }
    return PJ_FALSE;
}

static pjsip_module mod_inv_test =
{
    NULL, NULL,                          /* prev, next */
    {(char*)"mod-inv-test", 13},         /* name */
    -1,                                  /* id */
    PJSIP_MOD_PRIORITY_APPLICATION,      /* priority (>=0) */
    NULL,                                /* load */
    NULL,                                /* start */
    NULL,                                /* stop */
    NULL,                                /* unload */
    &uas_on_rx_request,                  /* on_rx_request */
    NULL,                                /* on_rx_response */
    NULL,                                /* on_tx_request */
    NULL,                                /* on_tx_response */
    NULL                                 /* on_tsx_state */
};

/* ------------------------------------------------------------------ */
/* Start the UAC: create dialog + invite session + send INVITE.        */
/* ------------------------------------------------------------------ */
static pj_status_t uac_start(void)
{
    pj_str_t local_uri, remote_uri;
    pjsip_tx_data *tdata = NULL;
    pjsip_tpselector tp_sel;
    pj_status_t rc;

    SET_STR(local_uri, "sip:user@10.0.2.15:15062");
    SET_STR(remote_uri, "sip:user@10.0.2.15:15062");

    rc = pjsip_dlg_create_uac(g_ua, &local_uri, NULL, &remote_uri, NULL,
                              &g_uac_dlg);
    if (rc != PJ_SUCCESS)
        return rc;

    /* Force the INVITE out on our UDP transport (loopback). */
    pj_bzero(&tp_sel, sizeof(tp_sel));
    tp_sel.type = PJSIP_TPSELECTOR_TRANSPORT;
    tp_sel.u.transport = g_tp;
    pjsip_dlg_set_transport(g_uac_dlg, &tp_sel);

    rc = pjsip_inv_create_uac(g_uac_dlg, g_offer_sdp, 0, &g_uac_inv);
    if (rc != PJ_SUCCESS)
        return rc;

    rc = pjsip_inv_invite(g_uac_inv, &tdata);
    if (rc != PJ_SUCCESS)
        return rc;

    rc = pjsip_inv_send_msg(g_uac_inv, tdata);
    printf("pj_sip_inv: UAC INVITE sent (rc=%d)\\
", rc);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Test entry                                                          */
/* ------------------------------------------------------------------ */
int pj_sip_inv_test_run(void)
{
    pj_status_t rc;
    pj_pool_t *pool;
    pj_sockaddr_in local;
    pj_sockaddr host;
    pjsip_ua_init_param ua_prm;
    pjsip_inv_callback inv_cb;
    pj_time_val tmo;
    int i;
    int pass = 0;

    printf("\\
=== PJSIP INVITE loopback test ===\\
");

    rc = pj_init();
    if (rc != PJ_SUCCESS) {
        printf("pj_sip_inv: pj_init failed (%d)\\
", rc);
        return -1;
    }
    pj_caching_pool_init(&g_cp, &pj_pool_factory_default_policy, 0);

    rc = pjsip_endpt_create(&g_cp.factory, "mps2-an505-inv", &g_endpt);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: endpt failed (%d)\\
", rc); return -1; }

    rc = pjsip_tsx_layer_init_module(g_endpt);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: tsx failed (%d)\\
", rc); return -1; }

    pj_bzero(&ua_prm, sizeof(ua_prm));
    rc = pjsip_ua_init_module(g_endpt, &ua_prm);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: ua failed (%d)\\
", rc); return -1; }
    g_ua = pjsip_ua_instance();

    pj_bzero(&inv_cb, sizeof(inv_cb));
    inv_cb.on_state_changed = &inv_on_state_changed;
    rc = pjsip_inv_usage_init(g_endpt, &inv_cb);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: inv_usage failed (%d)\\
", rc); return -1; }

    rc = pjsip_endpt_register_module(g_endpt, &mod_inv_test);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: mod failed (%d)\\
", rc); return -1; }

    /* UDP transport bound to local IP:SIP_INV_PORT. */
    rc = pj_gethostip(pj_AF_INET(), &host);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: gethostip failed (%d)\\
", rc); return -1; }
    pj_sockaddr_print(&host, g_local_ip, sizeof(g_local_ip), 0);
    printf("pj_sip_inv: local IP = %s\\
", g_local_ip);

    pj_sockaddr_in_init(&local, NULL, (pj_uint16_t)SIP_INV_PORT);
    local.sin_addr = host.ipv4.sin_addr;
    rc = pjsip_udp_transport_start(g_endpt, &local, NULL, 1, &g_tp);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: udp failed (%d)\\
", rc); return -1; }
    printf("pj_sip_inv: UDP transport up on %s:%d\\
", g_local_ip, SIP_INV_PORT);

    /* Build local SDP offer (UAC) and answer capability (UAS). */
    pool = pj_pool_create(&g_cp.factory, "sdp", 1024, 1024, NULL);
    g_offer_sdp = create_audio_sdp(pool, 4000);
    g_ans_sdp   = create_audio_sdp(pool, 4002);

    /* Fire the UAC INVITE. */
    rc = uac_start();
    if (rc != PJ_SUCCESS) {
        printf("pj_sip_inv: uac_start failed (%d)\\
", rc);
        return -1;
    }

    /* Run the event loop until both sessions confirm. */
    tmo.sec = 0;
    tmo.msec = 100;
    for (i = 0; i < MAX_LOOP && !(g_uac_done && g_uas_done); ++i) {
        pjsip_endpt_handle_events(g_endpt, &tmo);
    }

    if (g_uac_done && g_uas_done) {
        const pjmedia_sdp_session *loc = NULL, *rem = NULL;
        char buf[512];
        int n;
        printf("pj_sip_inv: INVITE CONFIRMED (UAC + UAS)\\
");
        if (g_uac_inv && g_uac_inv->neg &&
            pjmedia_sdp_neg_get_active_local(g_uac_inv->neg, &loc) == PJ_SUCCESS &&
            pjmedia_sdp_neg_get_active_remote(g_uac_inv->neg, &rem) == PJ_SUCCESS) {
            n = pjmedia_sdp_print(loc, buf, (pj_size_t)sizeof(buf));
            if (n > 0) {
                buf[n] = 0;
                printf("pj_sip_inv: UAC negotiated local SDP:\\
%s\\
", buf);
            }
            n = pjmedia_sdp_print(rem, buf, (pj_size_t)sizeof(buf));
            if (n > 0) {
                buf[n] = 0;
                printf("pj_sip_inv: UAC negotiated remote SDP:\\
%s\\
", buf);
            }
        }
        pass = 1;
    } else {
        printf("pj_sip_inv: FAILED (uac_done=%d uas_done=%d)\\
",
               g_uac_done, g_uas_done);
    }

    /* Teardown. */
    if (g_uac_inv) pjsip_inv_terminate(g_uac_inv, 0, PJ_FALSE);
    if (g_uas_inv) pjsip_inv_terminate(g_uas_inv, 0, PJ_FALSE);
    pjsip_endpt_destroy(g_endpt);
    pj_caching_pool_destroy(&g_cp);
    pj_shutdown();

    printf("pj_sip_inv: %s\\
", pass ? "ALL PASSED" : "FAILED");
    return pass ? 0 : -1;
}

