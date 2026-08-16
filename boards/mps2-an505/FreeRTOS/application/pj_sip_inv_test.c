/*
 * pj_sip_inv_test.c - PJSIP INVITE + SDP loopback self-test (stage 4).
 *
 * This version works at the TRANSACTION level (the pjsip_ua dialog layer is
 * not yet stable on this port), but it still exercises:
 *   - an INVITE request with an SDP offer body (audio/PCMU)
 *   - a server-side module that parses the offer and answers 200 OK with an
 *     SDP answer body
 *   - pjmedia SDP parse + print (pjmedia_sdp_parse / pjmedia_sdp_print)
 *   - the full client/server transaction round trip over the UDP transport
 *
 * The UAC sends INVITE to the board's own UDP transport (loopback); no
 * external server needed.
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
#include <pjsip-ua/sip_inv.h>
#include <pjmedia/sdp.h>

#define SIP_INV_PORT    15062
#define MAX_LOOP        100         /* 100 x 100ms = 10s max wait */

static pj_caching_pool   g_cp;
static pjsip_endpoint   *g_endpt;
static pjsip_transport  *g_tp;

static volatile int       g_tx_done;
static pj_status_t        g_tx_status;
static int                g_rx_code;
static char               g_answer_sdp[512];  /* SDP answer from UAS */
static int                g_answer_len;

static pjmedia_sdp_session *g_offer_sdp;      /* UAC offer */

static char g_local_ip[PJ_INET_ADDRSTRLEN];
static char g_offer_buf[512];                 /* serialized offer (static:
                                                 valid during retransmits) */

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
    /* g_local_ip is a char array; SET_STR would use sizeof() for its length,
     * so set the length explicitly from the actual string. */
    pj_strset(&sess->origin.addr, g_local_ip,
              (pj_ssize_t)pj_ansi_strlen(g_local_ip));
    SET_STR(sess->name, "invite-test");

    sess->conn = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_conn);
    SET_STR(sess->conn->net_type, "IN");
    SET_STR(sess->conn->addr_type, "IP4");
    pj_strset(&sess->conn->addr, g_local_ip,
              (pj_ssize_t)pj_ansi_strlen(g_local_ip));
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
/* pjsip_msg_body helpers for a text (SDP) body.                       */
/* ------------------------------------------------------------------ */
static int sdp_body_print(struct pjsip_msg_body *mb, char *buf, pj_size_t size)
{
    if ((pj_size_t)mb->len >= size)
        return -1;
    memcpy(buf, mb->data, (size_t)mb->len);
    buf[mb->len] = '\0';
    return (int)mb->len;
}

static void *sdp_body_clone(pj_pool_t *pool, const void *data, unsigned len)
{
    void *p = pj_pool_alloc(pool, len);
    if (p)
        memcpy(p, data, len);
    return p;
}

/* ------------------------------------------------------------------ */
/* UAC send callback: fired when the INVITE transaction changes state. */
/* ------------------------------------------------------------------ */
static void inv_on_state_changed(pjsip_inv_session *inv, pjsip_event *e)
{
    /* Not used: this test does not create INVITE sessions.  Required
     * non-NULL by pjsip_inv_usage_init(). */
    PJ_UNUSED_ARG(inv);
    PJ_UNUSED_ARG(e);
}

static void tx_cb(void *token, pjsip_event *e)
{
    PJ_UNUSED_ARG(token);

    if (e->type == PJSIP_EVENT_TSX_STATE &&
        e->body.tsx_state.type == PJSIP_EVENT_RX_MSG)
    {
        pjsip_rx_data *rdata = e->body.tsx_state.src.rdata;
        if (rdata) {
            g_rx_code = rdata->msg_info.msg->line.status.code;
            if (g_rx_code >= 200) {
                pjsip_rdata_sdp_info *si = pjsip_rdata_get_sdp_info(rdata);
                if (si && si->sdp) {
                    int n = pjmedia_sdp_print(si->sdp, g_answer_sdp,
                                              (pj_size_t)sizeof(g_answer_sdp));
                    if (n > 0) {
                        g_answer_sdp[n] = 0;
                        g_answer_len = n;
                    }
                }
                g_tx_status = PJ_SUCCESS;
                g_tx_done = 1;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* UAS module: parse the INVITE SDP offer, answer 200 OK with an SDP   */
/* answer (the offer echoed back with our media port).                 */
/* ------------------------------------------------------------------ */
static pjsip_module mod_inv_test;

static pj_status_t uas_on_rx_request(pjsip_rx_data *rdata)
{
    if (pjsip_method_cmp(&rdata->msg_info.msg->line.req.method,
                         &pjsip_invite_method) == 0) {
        pjsip_rdata_sdp_info *si;
        pjsip_msg_body body;
        pjmedia_sdp_session *answer = NULL;
        char abuf[512];
        int alen = 0;
        pj_status_t rc;

        /* Parse the SDP offer in the INVITE. */
        si = pjsip_rdata_get_sdp_info(rdata);
        if (si && si->sdp) {
            answer = pjmedia_sdp_session_clone(rdata->tp_info.pool, si->sdp);
            if (answer && answer->media_count > 0)
                answer->media[0]->desc.port = 4002;   /* our RTP port */
        }
        if (answer) {
            alen = pjmedia_sdp_print(answer, abuf, (pj_size_t)sizeof(abuf));
            if (alen <= 0) {
                printf("pj_sip_inv: UAS sdp print failed (%d)\r\n", alen);
                alen = 0;
            }
        }
        printf("pj_sip_inv: UAS got INVITE, offer %s, answering 200 (%d bytes SDP)\r\n",
               si && si->sdp ? "parsed" : "MISSING", alen);

        if (alen <= 0) {
            rc = pjsip_endpt_respond(g_endpt, &mod_inv_test, rdata,
                                     488, NULL, NULL, NULL, NULL);
            return PJ_TRUE;
        }

        pj_bzero(&body, sizeof(body));
        pjsip_media_type_init2(&body.content_type, "application", "sdp");
        body.data = abuf;
        body.len = (unsigned)alen;
        body.print_body = &sdp_body_print;
        body.clone_data = &sdp_body_clone;

        rc = pjsip_endpt_respond(g_endpt, &mod_inv_test, rdata,
                                 200, NULL, NULL, &body, NULL);
        printf("pj_sip_inv: UAS 200 response sent (rc=%d)\r\n", rc);
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
/* UAC: build INVITE with SDP offer and send it.                       */
/* ------------------------------------------------------------------ */
static pj_status_t uac_send_invite(void)
{
    pj_str_t target, from, to;
    pjsip_tx_data *tdata = NULL;
    pjsip_msg_body body;
    pjsip_tpselector tp_sel;
    pj_status_t rc;

    SET_STR(target, "sip:user@10.0.2.15:15062");
    SET_STR(from,   "sip:uac@10.0.2.15");
    SET_STR(to,     "sip:user@10.0.2.15:15062");

    rc = pjsip_endpt_create_request(g_endpt, &pjsip_invite_method,
                                    &target, &from, &to, NULL,
                                    NULL, -1, NULL, &tdata);
    if (rc != PJ_SUCCESS)
        return rc;

    /* Serialize the SDP offer (static buffer so it survives retransmits). */
    g_offer_buf[0] = 0;
    {
        int olen = pjmedia_sdp_print(g_offer_sdp, g_offer_buf,
                                     (pj_size_t)sizeof(g_offer_buf));
        if (olen <= 0)
            return PJ_EINVAL;
        if (olen >= (int)sizeof(g_offer_buf))
            olen = (int)sizeof(g_offer_buf) - 1;
        g_offer_buf[olen] = '\0';
        printf("pj_sip_inv: UAC offer (%d bytes)\r\n", olen);
    }

    /* Attach the SDP offer body.  Use the exact serialized length (the
     * print function does not NUL-terminate). */
    pj_bzero(&body, sizeof(body));
    pjsip_media_type_init2(&body.content_type, "application", "sdp");
    body.data = g_offer_buf;
    body.len = (unsigned)pj_ansi_strlen(g_offer_buf);
    body.print_body = &sdp_body_print;
    body.clone_data = &sdp_body_clone;
    tdata->msg->body = &body;

    /* Force the INVITE out on our UDP transport (loopback). */
    pj_bzero(&tp_sel, sizeof(tp_sel));
    tp_sel.type = PJSIP_TPSELECTOR_TRANSPORT;
    tp_sel.u.transport = g_tp;
    pjsip_tx_data_set_transport(tdata, &tp_sel);

    rc = pjsip_endpt_send_request(g_endpt, tdata, 3000, NULL, &tx_cb);
    printf("pj_sip_inv: UAC INVITE sent (rc=%d)\r\n", rc);
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
    pj_time_val tmo;
    int i;
    int pass = 0;

    printf("\r\n=== PJSIP INVITE + SDP loopback test ===\r\n");

    rc = pj_init();
    if (rc != PJ_SUCCESS) {
        printf("pj_sip_inv: pj_init failed (%d)\r\n", rc);
        return -1;
    }
    pj_caching_pool_init(&g_cp, &pj_pool_factory_default_policy, 0);

    rc = pjsip_endpt_create(&g_cp.factory, "mps2-an505-inv", &g_endpt);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: endpt failed (%d)\r\n", rc); return -1; }

    rc = pjsip_tsx_layer_init_module(g_endpt);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: tsx failed (%d)\r\n", rc); return -1; }

    /* Register the INVITE usage module.  Even though this test does not
     * create INVITE sessions, pjsip_rdata_get_sdp_info() needs mod_invite's
     * module id to cache the parsed SDP in the rdata. */
    {
        pjsip_inv_callback inv_cb;
        pj_bzero(&inv_cb, sizeof(inv_cb));
        inv_cb.on_state_changed = &inv_on_state_changed;
        rc = pjsip_inv_usage_init(g_endpt, &inv_cb);
        if (rc != PJ_SUCCESS) { printf("pj_sip_inv: inv_usage failed (%d)\r\n", rc); return -1; }
    }

    rc = pjsip_endpt_register_module(g_endpt, &mod_inv_test);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: mod failed (%d)\r\n", rc); return -1; }

    /* UDP transport bound to local IP:SIP_INV_PORT. */
    rc = pj_gethostip(pj_AF_INET(), &host);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: gethostip failed (%d)\r\n", rc); return -1; }
    pj_sockaddr_print(&host, g_local_ip, sizeof(g_local_ip), 0);
    printf("pj_sip_inv: local IP = %s\r\n", g_local_ip);

    pj_sockaddr_in_init(&local, NULL, (pj_uint16_t)SIP_INV_PORT);
    local.sin_addr = host.ipv4.sin_addr;
    rc = pjsip_udp_transport_start(g_endpt, &local, NULL, 1, &g_tp);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: udp failed (%d)\r\n", rc); return -1; }
    printf("pj_sip_inv: UDP transport up on %s:%d\r\n", g_local_ip, SIP_INV_PORT);

    /* Build the local SDP offer. */
    pool = pj_pool_create(&g_cp.factory, "sdp", 1024, 1024, NULL);
    g_offer_sdp = create_audio_sdp(pool, 4000);
    if (!g_offer_sdp) {
        printf("pj_sip_inv: create_audio_sdp failed\r\n");
        return -1;
    }

    /* Fire the UAC INVITE. */
    g_tx_done = 0;
    g_tx_status = PJ_EPENDING;
    g_rx_code = 0;
    g_answer_len = 0;
    rc = uac_send_invite();
    if (rc != PJ_SUCCESS) {
        printf("pj_sip_inv: uac_send_invite failed (%d)\r\n", rc);
        return -1;
    }

    /* Run the event loop until the transaction completes. */
    tmo.sec = 0;
    tmo.msec = 100;
    for (i = 0; i < MAX_LOOP && !g_tx_done; ++i) {
        pjsip_endpt_handle_events(g_endpt, &tmo);
    }

    if (g_tx_done && g_tx_status == PJ_SUCCESS && g_rx_code == 200) {
        printf("pj_sip_inv: INVITE succeeded, got %d\r\n", g_rx_code);
        if (g_answer_len > 0) {
            printf("pj_sip_inv: UAC received SDP answer:\r\n%s\r\n", g_answer_sdp);
            if (strstr(g_answer_sdp, "m=audio") != NULL)
                pass = 1;
        }
    } else {
        printf("pj_sip_inv: FAILED (done=%d status=%d code=%d)\r\n",
               g_tx_done, g_tx_status, g_rx_code);
    }

    /* Teardown. */
    pjsip_endpt_destroy(g_endpt);
    pj_caching_pool_destroy(&g_cp);
    pj_shutdown();

    printf("pj_sip_inv: %s\r\n", pass ? "ALL PASSED" : "FAILED");
    return pass ? 0 : -1;
}
