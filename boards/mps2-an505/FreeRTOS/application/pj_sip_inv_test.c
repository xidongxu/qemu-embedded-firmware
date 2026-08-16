/*
 * pj_sip_inv_test.c - PJSIP INVITE session loopback self-test (stage 5).
 *
 * Full INVITE session test over the dialog + UA layer:
 *   - UAC: pjsip_dlg_create_uac() -> pjsip_inv_create_uac() (SDP offer) ->
 *          pjsip_dlg_set_transport() -> pjsip_inv_invite()/send_msg()
 *   - UAS: a pjsip module answers the initial INVITE:
 *          pjsip_dlg_create_uas_and_inc_lock() -> pjsip_inv_create_uas() ->
 *          pjsip_inv_answer(200, SDP answer) -> pjsip_inv_send_msg()
 *   - Both sessions reach PJSIP_INV_STATE_CONFIRMED (automatic ACK).
 *
 * CRITICAL call-order note (the old stage-4 "dialog corruption" bug):
 *   pjsip_dlg_set_transport() internally does inc_lock/dec_lock, and
 *   pjsip_dlg_dec_lock() DESTROYS the dialog when sess_count==0 &&
 *   tsx_count==0.  So the UAC must create the INVITE session
 *   (pjsip_inv_create_uac, which bumps sess_count to 1) BEFORE calling
 *   pjsip_dlg_set_transport().
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
#include <pj/sock_select.h>
#include <pj/addr_resolv.h>
#include <pjsip.h>
#include <pjsip/sip_dialog.h>
#include <pjsip/sip_ua_layer.h>
#include <pjsip-ua/sip_inv.h>
#include <pjsip-ua/sip_100rel.h>
#include <pjsip-ua/sip_timer.h>
#include <pjmedia/sdp.h>
#include <pjmedia/rtp.h>
#include <pjmedia/alaw_ulaw.h>

#include "mic.h"
#include "audio.h"

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
static volatile int       g_failed;

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
/* INVITE session state callback.                                      */
/* ------------------------------------------------------------------ */
static const char *inv_state_name(pjsip_inv_state st)
{
    switch (st) {
    case PJSIP_INV_STATE_NULL:         return "NULL";
    case PJSIP_INV_STATE_CALLING:      return "CALLING";
    case PJSIP_INV_STATE_INCOMING:     return "INCOMING";
    case PJSIP_INV_STATE_EARLY:        return "EARLY";
    case PJSIP_INV_STATE_CONNECTING:   return "CONNECTING";
    case PJSIP_INV_STATE_CONFIRMED:    return "CONFIRMED";
    case PJSIP_INV_STATE_DISCONNECTED: return "DISCONNECTED";
    default:                           return "?";
    }
}

static void inv_on_state_changed(pjsip_inv_session *inv, pjsip_event *e)
{
    const char *who = (inv == g_uac_inv) ? "UAC" :
                      (inv == g_uas_inv) ? "UAS" : "?";
    printf("pj_sip_inv: [%s] state -> %d (%s)\r\n", who,
           (int)inv->state, inv_state_name(inv->state));

    if (inv->state == PJSIP_INV_STATE_CONFIRMED) {
        if (inv == g_uac_inv) g_uac_done = 1;
        if (inv == g_uas_inv) g_uas_done = 1;
    } else if (inv->state == PJSIP_INV_STATE_DISCONNECTED) {
        if (inv == g_uac_inv || inv == g_uas_inv)
            g_failed = 1;
    }
    PJ_UNUSED_ARG(e);
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
        pjsip_dialog *dlg = NULL;
        pj_str_t uas_contact;
        pj_status_t rc;

        /* The UAS must advertise a contact that includes the local port,
         * otherwise the UAC's ACK (and future requests) would go to the
         * default port (5060) instead of our UDP port (15062). */
        pj_strset(&uas_contact, "sip:user@10.0.2.15:15062",
                  (pj_ssize_t)pj_ansi_strlen("sip:user@10.0.2.15:15062"));

        /* Create UAS dialog (holds the dialog lock). */
        rc = pjsip_dlg_create_uas_and_inc_lock(g_ua, rdata, &uas_contact,
                                               &dlg);
        if (rc != PJ_SUCCESS) {
            printf("pj_sip_inv: UAS dlg_create failed (%d)\r\n", rc);
            g_failed = 1;
            return PJ_TRUE;
        }

        /* Create the UAS invite session with our answer capability. */
        rc = pjsip_inv_create_uas(dlg, rdata, g_ans_sdp, 0, &g_uas_inv);
        if (rc != PJ_SUCCESS) {
            printf("pj_sip_inv: UAS inv_create failed (%d)\r\n", rc);
            pjsip_dlg_dec_lock(dlg);
            g_failed = 1;
            return PJ_TRUE;
        }

        /* We are done with the dialog lock (the session holds its own ref). */
        pjsip_dlg_dec_lock(dlg);

        /* Create the 200 OK response with SDP answer.  Use
         * pjsip_inv_initial_answer() for the FIRST answer (it sets
         * inv->last_answer); pjsip_inv_answer() clones last_answer and
         * would assert if it is NULL.  local_sdp was given at create_uas,
         * so pass NULL here. */
        rc = pjsip_inv_initial_answer(g_uas_inv, rdata, 200, NULL, NULL,
                                      &tdata);
        if (rc == PJ_SUCCESS)
            rc = pjsip_inv_send_msg(g_uas_inv, tdata);
        printf("pj_sip_inv: UAS answered 200 OK (rc=%d)\r\n", rc);
        if (rc != PJ_SUCCESS)
            g_failed = 1;
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

    /* 1) Create the UAC dialog. */
    rc = pjsip_dlg_create_uac(g_ua, &local_uri, NULL, &remote_uri, NULL,
                              &g_uac_dlg);
    if (rc != PJ_SUCCESS)
        return rc;
    printf("pj_sip_inv: UAC dialog created (%p)\r\n", (void*)g_uac_dlg);

    /* 2) Create the UAC invite session FIRST.  This increments the
     *    dialog's session count so that pjsip_dlg_set_transport()'s
     *    internal dec_lock() does NOT destroy the dialog (see file header).
     *    It also creates the SDP negotiator with our offer. */
    rc = pjsip_inv_create_uac(g_uac_dlg, g_offer_sdp, 0, &g_uac_inv);
    if (rc != PJ_SUCCESS)
        return rc;
    printf("pj_sip_inv: UAC invite session created (%p), sess_count=%d\r\n",
           (void*)g_uac_inv, (int)g_uac_dlg->sess_count);

    /* 3) Force the INVITE out on our UDP transport (loopback).  Safe now
     *    that the dialog has a session. */
    pj_bzero(&tp_sel, sizeof(tp_sel));
    tp_sel.type = PJSIP_TPSELECTOR_TRANSPORT;
    tp_sel.u.transport = g_tp;
    rc = pjsip_dlg_set_transport(g_uac_dlg, &tp_sel);
    if (rc != PJ_SUCCESS)
        return rc;

    /* 4) Build the INVITE request and send it. */
    rc = pjsip_inv_invite(g_uac_inv, &tdata);
    if (rc != PJ_SUCCESS)
        return rc;

    rc = pjsip_inv_send_msg(g_uac_inv, tdata);
    printf("pj_sip_inv: UAC INVITE sent (rc=%d)\r\n", rc);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Media phase: once the INVITE session is CONFIRMED, start real       */
/* full-duplex RTP/PCMU media between UAC and UAS using the RTP ports  */
/* that were negotiated in SDP (UAC offer port, UAS answer port).      */
/* ------------------------------------------------------------------ */
#define MEDIA_RATE       8000
#define MEDIA_FMT        MIC_FORMAT_S16
#define MFRAME_SAMPLES   80          /* 10 ms @ 8 kHz */
#define MFRAME_BYTES     (MFRAME_SAMPLES * 2)
#define MEDIA_FRAMES     200         /* 200 x 10 ms = 2 s */
#define MEDIA_PCM_BYTES  (MEDIA_FRAMES * MFRAME_BYTES)   /* 32000 bytes */

typedef struct media_ep {
    pj_sock_t         rx, tx;
    pj_sockaddr_in    peer;
    pjmedia_rtp_session rtp;
    const uint8_t    *cap;
    uint8_t          *play;
    pj_uint32_t       ssrc;
} media_ep;

static uint8_t s_mcap_uac[MEDIA_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_mcap_uas[MEDIA_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_mplay_uac[MEDIA_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_mplay_uas[MEDIA_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_mplay_uac_u8[MEDIA_PCM_BYTES / 2] __attribute__((aligned(64)));
static uint8_t s_mplay_uas_u8[MEDIA_PCM_BYTES / 2] __attribute__((aligned(64)));

static pj_status_t media_ep_open(media_ep *ep, const pj_sockaddr_in *peer_ip,
                                 pj_uint16_t rx_port, pj_uint16_t tx_port,
                                 pj_uint32_t ssrc)
{
    pj_sockaddr_in local;
    pj_status_t rc;

    rc = pj_sock_socket(PJ_AF_INET, PJ_SOCK_DGRAM, 0, &ep->rx);
    if (rc != PJ_SUCCESS) return rc;
    pj_sockaddr_in_init(&local, NULL, rx_port);
    rc = pj_sock_bind(ep->rx, &local, sizeof(local));
    if (rc != PJ_SUCCESS) return rc;
    rc = pj_sock_socket(PJ_AF_INET, PJ_SOCK_DGRAM, 0, &ep->tx);
    if (rc != PJ_SUCCESS) return rc;
    pj_sockaddr_in_init(&ep->peer, NULL, tx_port);
    ep->peer.sin_addr = peer_ip->sin_addr;
    ep->ssrc = ssrc;
    return pjmedia_rtp_session_init(&ep->rtp, 0, ssrc);
}

static pj_status_t media_ep_send(media_ep *ep, int frame)
{
    const uint8_t *fp = ep->cap + frame * MFRAME_BYTES;
    pj_uint8_t ulaw[MFRAME_SAMPLES], pkt[256];
    const void *rtphdr = NULL;
    int hdrlen = 0, k;
    pj_status_t rc;

    for (k = 0; k < MFRAME_SAMPLES; ++k) {
        pj_int16_t s;
        memcpy(&s, fp + k * 2, 2);
        ulaw[k] = pjmedia_linear2ulaw(s);
    }
    rc = pjmedia_rtp_encode_rtp(&ep->rtp, 0, 0, MFRAME_SAMPLES,
                                MFRAME_SAMPLES, &rtphdr, &hdrlen);
    if (rc != PJ_SUCCESS) return rc;
    memcpy(pkt, rtphdr, (size_t)hdrlen);
    memcpy(pkt + hdrlen, ulaw, sizeof(ulaw));
    {
        pj_ssize_t len = hdrlen + (int)sizeof(ulaw);
        return pj_sock_sendto(ep->tx, pkt, &len, 0, &ep->peer, sizeof(ep->peer));
    }
}

static int media_ep_recv(media_ep *ep, int frame)
{
    pj_uint8_t buf[256];
    pj_ssize_t len = (pj_ssize_t)sizeof(buf);
    const pjmedia_rtp_hdr *hdr = NULL;
    const void *payload = NULL;
    unsigned plen = 0;
    pjmedia_rtp_status seq_st;
    const pj_uint8_t *ul;
    uint8_t *out = ep->play + frame * MFRAME_BYTES;
    pj_fd_set_t rfds;
    pj_time_val stmo;
    int k;
    pj_status_t rc;

    PJ_FD_ZERO(&rfds);
    PJ_FD_SET(ep->rx, &rfds);
    stmo.sec = 2; stmo.msec = 0;
    if (pj_sock_select((int)ep->rx + 1, &rfds, NULL, NULL, &stmo) <= 0 ||
        !PJ_FD_ISSET(ep->rx, &rfds))
        return -1;
    rc = pj_sock_recvfrom(ep->rx, buf, &len, 0, NULL, NULL);
    if (rc != PJ_SUCCESS) return -1;
    rc = pjmedia_rtp_decode_rtp(&ep->rtp, buf, (int)len, &hdr, &payload, &plen);
    if (rc != PJ_SUCCESS) return -1;
    pjmedia_rtp_session_update(&ep->rtp, hdr, &seq_st);
    if (seq_st.status.flag.bad) return -1;
    ul = (const pj_uint8_t*)payload;
    if (plen > MFRAME_SAMPLES) plen = MFRAME_SAMPLES;
    for (k = 0; k < (int)plen; ++k) {
        pj_int16_t s = (pj_int16_t)pjmedia_ulaw2linear(ul[k]);
        out[k * 2]     = (uint8_t)(s & 0xFF);
        out[k * 2 + 1] = (uint8_t)((s >> 8) & 0xFF);
    }
    return 0;
}

static void media_pcm_to_u8(const uint8_t *s16, uint8_t *u8, int bytes)
{
    int i;
    for (i = 0; i < bytes / 2; ++i) {
        pj_int16_t s;
        memcpy(&s, s16 + i * 2, 2);
        u8[i] = (uint8_t)(128 + (s >> 8));
    }
}

static long media_peak(const uint8_t *pcm, int bytes)
{
    pj_int32_t peak = 0;
    int i;
    for (i = 0; i < bytes / 2; ++i) {
        pj_int16_t s;
        memcpy(&s, pcm + i * 2, 2);
        if (s < 0) s = (pj_int16_t)(-s);
        if (s > peak) peak = s;
    }
    return (long)peak;
}

/* Run media on the negotiated RTP ports. uac_rx / uas_rx are the RTP ports
 * from SDP offer/answer; uac sends to uas_rx, uas sends to uac_rx. */
static int run_confirmed_media(const pj_sockaddr_in *host_ip,
                               pj_uint16_t uac_rx, pj_uint16_t uas_rx)
{
    media_ep epU, epS;
    int i, okU = 0, okS = 0, badU = 0, badS = 0;

    memset(&epU, 0, sizeof(epU));
    memset(&epS, 0, sizeof(epS));
    epU.rx = epU.tx = PJ_INVALID_SOCKET;
    epS.rx = epS.tx = PJ_INVALID_SOCKET;

    printf("pj_sip_inv: media phase on RTP ports uac=%u uas=%u\r\n",
           (unsigned)uac_rx, (unsigned)uas_rx);

    if (media_ep_open(&epU, host_ip, uac_rx, uas_rx, 0xAAA00002) != PJ_SUCCESS ||
        media_ep_open(&epS, host_ip, uas_rx, uac_rx, 0xBBB00002) != PJ_SUCCESS) {
        printf("pj_sip_inv: media socket open failed\r\n");
        return -1;
    }
    epU.cap = s_mcap_uac; epU.play = s_mplay_uac;
    epS.cap = s_mcap_uas; epS.play = s_mplay_uas;

    /* Capture one 2 s source per endpoint. */
    if (!mic_capture(s_mcap_uac, MEDIA_PCM_BYTES, MEDIA_RATE, MEDIA_FMT, 20000000UL)) {
        printf("pj_sip_inv: mic capture UAC failed\r\n");
        return -1;
    }
    if (!mic_capture(s_mcap_uas, MEDIA_PCM_BYTES, MEDIA_RATE, MEDIA_FMT, 20000000UL)) {
        printf("pj_sip_inv: mic capture UAS failed\r\n");
        return -1;
    }
    printf("pj_sip_inv: media source peaks uac=%ld uas=%ld\r\n",
           media_peak(s_mcap_uac, MEDIA_PCM_BYTES),
           media_peak(s_mcap_uas, MEDIA_PCM_BYTES));

    /* Full-duplex: UAC and UAS exchange PCMU RTP frames. */
    for (i = 0; i < MEDIA_FRAMES; ++i) {
        if (media_ep_send(&epU, i) == PJ_SUCCESS &&
            media_ep_send(&epS, i) == PJ_SUCCESS) {
            pj_thread_sleep(3);
            if (media_ep_recv(&epU, i) == 0) okU++; else badU++;
            if (media_ep_recv(&epS, i) == 0) okS++; else badS++;
        } else {
            printf("pj_sip_inv: media send failed at %d\r\n", i);
            break;
        }
    }

    printf("pj_sip_inv: media UAS->UAC ok=%d bad=%d, UAC->UAS ok=%d bad=%d\r\n",
           okU, badU, okS, badS);
    printf("pj_sip_inv: media playback peaks uac=%ld uas=%ld\r\n",
           media_peak(s_mplay_uac, MEDIA_PCM_BYTES),
           media_peak(s_mplay_uas, MEDIA_PCM_BYTES));

    /* Play both decoded streams (U8 for the QEMU wav backend). */
    media_pcm_to_u8(s_mplay_uac, s_mplay_uac_u8, MEDIA_PCM_BYTES);
    media_pcm_to_u8(s_mplay_uas, s_mplay_uas_u8, MEDIA_PCM_BYTES);
    printf("pj_sip_inv: playing UAC->speaker\r\n");
    audio_play(s_mplay_uac_u8, MEDIA_PCM_BYTES / 2, MEDIA_RATE, AUDIO_FORMAT_U8);
    pj_thread_sleep(2500);
    printf("pj_sip_inv: playing UAS->speaker\r\n");
    audio_play(s_mplay_uas_u8, MEDIA_PCM_BYTES / 2, MEDIA_RATE, AUDIO_FORMAT_U8);
    pj_thread_sleep(2500);
    audio_stop();

    if (epU.rx != PJ_INVALID_SOCKET) pj_sock_close(epU.rx);
    if (epU.tx != PJ_INVALID_SOCKET) pj_sock_close(epU.tx);
    if (epS.rx != PJ_INVALID_SOCKET) pj_sock_close(epS.rx);
    if (epS.tx != PJ_INVALID_SOCKET) pj_sock_close(epS.tx);

    if (okU == MEDIA_FRAMES && okS == MEDIA_FRAMES && badU == 0 && badS == 0)
        return 0;
    return -1;
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

    printf("\r\n=== PJSIP INVITE session loopback test ===\r\n");

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

    pj_bzero(&ua_prm, sizeof(ua_prm));
    rc = pjsip_ua_init_module(g_endpt, &ua_prm);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: ua failed (%d)\r\n", rc); return -1; }
    g_ua = pjsip_ua_instance();

    pj_bzero(&inv_cb, sizeof(inv_cb));
    inv_cb.on_state_changed = &inv_on_state_changed;
    rc = pjsip_inv_usage_init(g_endpt, &inv_cb);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: inv_usage failed (%d)\r\n", rc); return -1; }

    /* The INVITE usage calls pjsip_100rel_attach() (asserts mod_100rel.id>=0)
     * and pjsip_timer_*() on session create/answer, so register those
     * modules explicitly. */
    rc = pjsip_100rel_init_module(g_endpt);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: 100rel init failed (%d)\r\n", rc); return -1; }
    rc = pjsip_timer_init_module(g_endpt);
    if (rc != PJ_SUCCESS) { printf("pj_sip_inv: timer init failed (%d)\r\n", rc); return -1; }
    printf("pj_sip_inv: 100rel + session timer modules registered\r\n");

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

    /* Build local SDP offer (UAC) and answer capability (UAS). */
    pool = pj_pool_create(&g_cp.factory, "sdp", 1024, 1024, NULL);
    g_offer_sdp = create_audio_sdp(pool, 4000);
    g_ans_sdp   = create_audio_sdp(pool, 4002);
    if (!g_offer_sdp || !g_ans_sdp) {
        printf("pj_sip_inv: create_audio_sdp failed\r\n");
        return -1;
    }

    /* Fire the UAC INVITE. */
    g_uac_done = 0;
    g_uas_done = 0;
    g_failed = 0;
    rc = uac_start();
    if (rc != PJ_SUCCESS) {
        printf("pj_sip_inv: uac_start failed (%d)\r\n", rc);
        return -1;
    }

    /* Run the event loop until both sessions confirm. */
    tmo.sec = 0;
    tmo.msec = 100;
    for (i = 0; i < MAX_LOOP && !(g_uac_done && g_uas_done) && !g_failed; ++i) {
        pjsip_endpt_handle_events(g_endpt, &tmo);
    }

    if (g_uac_done && g_uas_done && !g_failed) {
        printf("pj_sip_inv: INVITE CONFIRMED (UAC + UAS)\r\n");
        pass = 1;

        /* Media phase: use the RTP ports negotiated in SDP.  The UAC offer
         * carries our (UAC) RTP port; the UAS answer carries the UAS RTP
         * port.  Each side receives on its own port and sends to the peer's
         * port. */
        {
            pj_uint16_t uac_rx, uas_rx;
            int media_pass;

            uac_rx = (g_offer_sdp && g_offer_sdp->media_count > 0) ?
                     g_offer_sdp->media[0]->desc.port : 4000;
            uas_rx = (g_ans_sdp && g_ans_sdp->media_count > 0) ?
                     g_ans_sdp->media[0]->desc.port : 4002;
            media_pass = run_confirmed_media(&host.ipv4, uac_rx, uas_rx);
            if (media_pass != 0) {
                printf("pj_sip_inv: media phase FAILED\r\n");
                pass = 0;
            } else {
                printf("pj_sip_inv: media phase ALL PASSED\r\n");
            }
        }
    } else {
        printf("pj_sip_inv: FAILED (uac_done=%d uas_done=%d failed=%d)\r\n",
               g_uac_done, g_uas_done, g_failed);
    }

    /* Teardown. */
    if (g_uac_inv) pjsip_inv_terminate(g_uac_inv, 0, PJ_FALSE);
    if (g_uas_inv) pjsip_inv_terminate(g_uas_inv, 0, PJ_FALSE);
    pjsip_endpt_destroy(g_endpt);
    pj_caching_pool_destroy(&g_cp);
    pj_shutdown();

    printf("pj_sip_inv: %s\r\n", pass ? "ALL PASSED" : "FAILED");
    return pass ? 0 : -1;
}
