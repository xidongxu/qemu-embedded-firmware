/*
 * pj_sip_dual_test.c - dual QEMU inter-instance SIP call test (stage 11).
 *
 * Two patched QEMU instances (mps2-an505) each run on their own user-mode
 * (slirp) network and both have guest IP 10.0.2.15.  To let them talk we
 * route ALL SIP + RTP through the slirp host gateway 10.0.2.2 using UDP
 * hostfwd port forwards on each instance.
 *
 *   Caller instance (UAC):
 *     QEMU:  -nic user,model=lan9118,hostfwd=udp::16062-:15062,hostfwd=udp::4000-:4000
 *     guest SIP  : bind  0.0.0.0:15062, published 10.0.2.2:16062
 *     dial target: sip:user@10.0.2.2:15062
 *     SDP offer  : c=IN IP4 10.0.2.2  m=audio 4000
 *     RTP        : rx bind 0.0.0.0:4000, tx -> 10.0.2.2:4002
 *
 *   Callee instance (UAS):
 *     QEMU:  -nic user,model=lan9118,hostfwd=udp::15062-:15062,hostfwd=udp::4002-:4002
 *     guest SIP  : bind  0.0.0.0:15062, published 10.0.2.2:15062
 *     contact    : sip:user@10.0.2.2:15062
 *     SDP answer : c=IN IP4 10.0.2.2  m=audio 4002
 *     RTP        : rx bind 0.0.0.0:4002, tx -> 10.0.2.2:4000
 *
 * Path A->B: A dials 10.0.2.2:15062 (hostfwd on B) and sends RTP to
 *            10.0.2.2:4002 (hostfwd on B).
 * Path B->A: B replies/ACKs to 10.0.2.2:16062 (hostfwd on A) and sends RTP
 *            to 10.0.2.2:4000 (hostfwd on A).
 *
 * Media runs on the negotiated RTP port through a pjmedia jitter buffer
 * (same as the stage-10 loopback test) with a 10 ms playback pace.
 */

#include <stdio.h>
#include <string.h>
#include "printf.h"
#include "FreeRTOS.h"
#include "task.h"
#include "pj_sip_dual_test.h"

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
#include <pjmedia/jbuf.h>
#include <pjmedia/alaw_ulaw.h>

#include "mic.h"
#include "audio.h"
#include "pj_rtcp_engine.h"

#if defined(PJ_DUAL_ROLE_CALLER) && defined(PJ_DUAL_ROLE_CALLEE)
#error "define exactly one of PJ_DUAL_ROLE_CALLER / PJ_DUAL_ROLE_CALLEE"
#endif
#if !defined(PJ_DUAL_ROLE_CALLER) && !defined(PJ_DUAL_ROLE_CALLEE)
#error "define PJ_DUAL_ROLE_CALLER or PJ_DUAL_ROLE_CALLEE"
#endif

/* ---- role-independent constants ---- */
#define GW_IP          "10.0.2.2"   /* slirp host gateway, both roles */
#define SIP_PORT       15062        /* guest-bound SIP port, both roles */
#define CALLEE_EXT_SIP 15062        /* callee published SIP port (hostfwd) */
#define CALLEE_RTP     4002         /* callee RTP port (hostfwd) */
#define CALLER_EXT_SIP 16062        /* caller published SIP port (hostfwd) */
#define CALLER_RTP     4000         /* caller RTP port (hostfwd) */
#define MAX_LOOP       600          /* 600 x 100ms = 60s max wait */

#if defined(PJ_DUAL_ROLE_CALLER)
#  define ROLE_NAME      "caller"
#  define MY_EXT_SIP     CALLER_EXT_SIP
#  define MY_RTP_PORT    CALLER_RTP
#  define PEER_RTP_PORT  CALLEE_RTP
#  define MY_SSRC        0xAAA00003
#  define PEER_SSRC      0xBBB00003
#else
#  define ROLE_NAME      "callee"
#  define MY_EXT_SIP     CALLEE_EXT_SIP
#  define MY_RTP_PORT    CALLEE_RTP
#  define PEER_RTP_PORT  CALLER_RTP
#  define MY_SSRC        0xBBB00003
#  define PEER_SSRC      0xAAA00003
#endif

/* RTCP rides the RTP port + 1 (RFC 3550); forwarded via hostfwd too. */
#define MY_RTCP_PORT    (MY_RTP_PORT + 1)
#define PEER_RTCP_PORT  (PEER_RTP_PORT + 1)

static pj_caching_pool    g_cp;
static pjsip_endpoint    *g_endpt;
static pjsip_transport   *g_tp;
static pjsip_user_agent  *g_ua;
#if defined(PJ_DUAL_ROLE_CALLER)
static pjsip_dialog      *g_dlg;
#endif
static pjsip_inv_session *g_inv;
static volatile int       g_confirmed;
static volatile int       g_failed;
static pjmedia_sdp_session *g_sdp;
static char               g_local_ip[PJ_INET_ADDRSTRLEN];

#define SET_STR(s, lit) do { (s).ptr = (char*)(lit); \
                             (s).slen = (pj_ssize_t)(sizeof(lit)-1); } while (0)

/* ------------------------------------------------------------------ */
/* Build an audio/PCMU SDP session with the given IP and RTP port.     */
/* ------------------------------------------------------------------ */
static pjmedia_sdp_session *create_audio_sdp(pj_pool_t *pool,
                                             const char *ip,
                                             pj_uint16_t port)
{
    pjmedia_sdp_session *sess;
    pjmedia_sdp_media *m;
    pjmedia_sdp_attr *a;
    pjmedia_sdp_rtpmap rtpmap;
    pj_str_t ip_str;

    sess = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_session);
    SET_STR(sess->origin.user, "mps2-an505");
    sess->origin.id = 0;
    sess->origin.version = 0;
    SET_STR(sess->origin.net_type, "IN");
    SET_STR(sess->origin.addr_type, "IP4");
    pj_strset(&ip_str, (char*)ip, (pj_ssize_t)pj_ansi_strlen(ip));
    sess->origin.addr = ip_str;
    SET_STR(sess->name, "dual-test");

    sess->conn = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_conn);
    SET_STR(sess->conn->net_type, "IN");
    SET_STR(sess->conn->addr_type, "IP4");
    sess->conn->addr = ip_str;
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
/* INVITE state callback.                                              */
/* ------------------------------------------------------------------ */
static void inv_on_state_changed(pjsip_inv_session *inv, pjsip_event *e)
{
    printf("pj_sip_dual[%s]: state -> %d\r\n", ROLE_NAME, (int)inv->state);
    if (inv->state == PJSIP_INV_STATE_CONFIRMED) {
        g_confirmed = 1;
    } else if (inv->state == PJSIP_INV_STATE_DISCONNECTED) {
        g_failed = 1;
    }
    PJ_UNUSED_ARG(e);
}

/* ------------------------------------------------------------------ */
/* UAS module (callee only): auto-answer INVITE with 200 OK + SDP.     */
/* ------------------------------------------------------------------ */
#if defined(PJ_DUAL_ROLE_CALLEE)
static pjsip_module mod_dual_uas;

static pj_status_t uas_on_rx_request(pjsip_rx_data *rdata)
{
    if (pjsip_method_cmp(&rdata->msg_info.msg->line.req.method,
                         &pjsip_invite_method) == 0) {
        pjsip_tx_data *tdata = NULL;
        pjsip_dialog *dlg = NULL;
        char contact[64];
        pj_str_t uas_contact;
        pj_status_t rc;

        pj_ansi_snprintf(contact, sizeof(contact),
                         "sip:user@%s:%d", GW_IP, MY_EXT_SIP);
        pj_strset(&uas_contact, contact,
                  (pj_ssize_t)pj_ansi_strlen(contact));

        rc = pjsip_dlg_create_uas_and_inc_lock(g_ua, rdata, &uas_contact,
                                               &dlg);
        if (rc != PJ_SUCCESS) {
            printf("pj_sip_dual[%s]: UAS dlg_create failed (%d)\r\n",
                   ROLE_NAME, rc);
            g_failed = 1;
            return PJ_TRUE;
        }
        rc = pjsip_inv_create_uas(dlg, rdata, g_sdp, 0, &g_inv);
        if (rc != PJ_SUCCESS) {
            printf("pj_sip_dual[%s]: UAS inv_create failed (%d)\r\n",
                   ROLE_NAME, rc);
            pjsip_dlg_dec_lock(dlg);
            g_failed = 1;
            return PJ_TRUE;
        }
        pjsip_dlg_dec_lock(dlg);

        rc = pjsip_inv_initial_answer(g_inv, rdata, 200, NULL, NULL, &tdata);
        if (rc == PJ_SUCCESS)
            rc = pjsip_inv_send_msg(g_inv, tdata);
        printf("pj_sip_dual[%s]: answered 200 OK (rc=%d)\r\n", ROLE_NAME, rc);
        if (rc != PJ_SUCCESS)
            g_failed = 1;
        return PJ_TRUE;
    }
    return PJ_FALSE;
}

static pjsip_module mod_dual_uas =
{
    NULL, NULL,                          /* prev, next */
    {(char*)"mod-dual-uas", 12},         /* name */
    -1,                                  /* id */
    PJSIP_MOD_PRIORITY_APPLICATION,      /* priority (>=0) */
    NULL, NULL, NULL, NULL,              /* load/start/stop/unload */
    &uas_on_rx_request,                  /* on_rx_request */
    NULL, NULL, NULL, NULL               /* on_rx_response/tx_* /tsx_state */
};
#endif /* PJ_DUAL_ROLE_CALLEE */

/* ------------------------------------------------------------------ */
/* UAC (caller only): dial the callee.                                 */
/* ------------------------------------------------------------------ */
#if defined(PJ_DUAL_ROLE_CALLER)
static pj_status_t uac_dial(void)
{
    char local_uri[64], remote_uri[64];
    pj_str_t luri, ruri;
    pjsip_tx_data *tdata = NULL;
    pjsip_tpselector tp_sel;
    pj_status_t rc;

    pj_ansi_snprintf(local_uri, sizeof(local_uri),
                     "sip:user@%s:%d", GW_IP, MY_EXT_SIP);
    pj_ansi_snprintf(remote_uri, sizeof(remote_uri),
                     "sip:user@%s:%d", GW_IP, CALLEE_EXT_SIP);
    SET_STR(luri, ""); SET_STR(ruri, "");
    pj_strset(&luri, local_uri, (pj_ssize_t)pj_ansi_strlen(local_uri));
    pj_strset(&ruri, remote_uri, (pj_ssize_t)pj_ansi_strlen(remote_uri));

    rc = pjsip_dlg_create_uac(g_ua, &luri, NULL, &ruri, NULL, &g_dlg);
    if (rc != PJ_SUCCESS) return rc;
    printf("pj_sip_dual[caller]: UAC dialog created (%p)\r\n", (void*)g_dlg);

    /* Create invite session FIRST (keeps dialog alive across
     * set_transport's internal dec_lock; see stage-5 gotcha 28). */
    rc = pjsip_inv_create_uac(g_dlg, g_sdp, 0, &g_inv);
    if (rc != PJ_SUCCESS) return rc;
    printf("pj_sip_dual[caller]: UAC invite session created (%p)\r\n",
           (void*)g_inv);

    pj_bzero(&tp_sel, sizeof(tp_sel));
    tp_sel.type = PJSIP_TPSELECTOR_TRANSPORT;
    tp_sel.u.transport = g_tp;
    rc = pjsip_dlg_set_transport(g_dlg, &tp_sel);
    if (rc != PJ_SUCCESS) return rc;

    rc = pjsip_inv_invite(g_inv, &tdata);
    if (rc != PJ_SUCCESS) return rc;
    rc = pjsip_inv_send_msg(g_inv, tdata);
    printf("pj_sip_dual[caller]: INVITE sent (rc=%d)\r\n", rc);
    return rc;
}
#endif /* PJ_DUAL_ROLE_CALLER */

/* ------------------------------------------------------------------ */
/* Media: one endpoint, RTP rx on MY_RTP_PORT, RTP tx to peer.         */
/* ------------------------------------------------------------------ */
#define MEDIA_RATE       8000
#define MEDIA_FMT        MIC_FORMAT_S16
#define MFRAME_SAMPLES   80          /* 10 ms @ 8 kHz */
#define MFRAME_BYTES     (MFRAME_SAMPLES * 2)
#define MEDIA_FRAMES     200         /* 200 x 10 ms = 2 s */
#define MEDIA_PCM_BYTES  (MEDIA_FRAMES * MFRAME_BYTES)   /* 32000 bytes */

typedef struct media_ep {
    pj_sock_t          rx, tx;
    pj_sockaddr_in     peer;
    pjmedia_rtp_session rtp;
    pjmedia_jbuf      *jb;
    pj_bool_t          have_jb;
    pj_sock_t          rtcp;          /* RTCP socket (bound, rx+tx)   */
    pj_sockaddr_in     peer_rtcp;     /* peer RTCP address            */
    rtcp_session       rtcps;         /* RTCP session (stats/RTT)     */
    const uint8_t     *cap;
    uint8_t           *play;
    pj_uint32_t        ssrc;
} media_ep;

static uint8_t s_mcap[MEDIA_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_mplay[MEDIA_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_mplay_u8[MEDIA_PCM_BYTES / 2] __attribute__((aligned(64)));

static pj_status_t media_ep_open(media_ep *ep, pj_uint16_t rx_port,
                                 const char *peer_ip, pj_uint16_t tx_port,
                                 pj_uint32_t ssrc, pj_pool_t *pool,
                                 const pj_str_t *name)
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
    {
        pj_str_t ip_str;
        pj_strset(&ip_str, (char*)peer_ip,
                  (pj_ssize_t)pj_ansi_strlen(peer_ip));
        rc = pj_inet_pton(pj_AF_INET(), &ip_str, &ep->peer.sin_addr);
    }
    if (rc != PJ_SUCCESS) return rc;
    ep->ssrc = ssrc;
    rc = pjmedia_rtp_session_init(&ep->rtp, 0, ssrc);
    if (rc != PJ_SUCCESS) return rc;

    rc = pjmedia_jbuf_create(pool, name, MFRAME_BYTES, 10, MEDIA_FRAMES + 10,
                             &ep->jb);
    if (rc == PJ_SUCCESS) {
        ep->have_jb = PJ_TRUE;
        /* Steady state is one frame in / one frame out every 10 ms, so a
         * fixed(1) prefetch keeps prefetching disabled and the depth at ~1.
         * Jitter absorption is handled in the application: rx_thread pre-fills
         * a few frames before play_thread starts (see run_dual_media). */
        pjmedia_jbuf_set_fixed(ep->jb, 1);
    }

    /* RTCP socket: bound to our RTCP port (RTP+1); SR/RR go to the peer's
     * RTCP port through the same hostfwd path as RTP. */
    ep->rtcp = PJ_INVALID_SOCKET;
    rc = pj_sock_socket(PJ_AF_INET, PJ_SOCK_DGRAM, 0, &ep->rtcp);
    if (rc != PJ_SUCCESS) return rc;
    pj_sockaddr_in_init(&local, NULL, MY_RTCP_PORT);
    rc = pj_sock_bind(ep->rtcp, &local, sizeof(local));
    if (rc != PJ_SUCCESS) return rc;
    pj_sockaddr_in_init(&ep->peer_rtcp, NULL, PEER_RTCP_PORT);
    {
        pj_str_t ip_str;
        pj_strset(&ip_str, (char*)peer_ip,
                  (pj_ssize_t)pj_ansi_strlen(peer_ip));
        rc = pj_inet_pton(pj_AF_INET(), &ip_str, &ep->peer_rtcp.sin_addr);
    }
    if (rc != PJ_SUCCESS) return rc;
    rtcp_init(&ep->rtcps, ssrc, MEDIA_RATE, PEER_SSRC, ROLE_NAME);
    return rc;
}

/* Encode and send one RTP frame (single transmission). */
static pj_status_t media_ep_send(media_ep *ep, int frame)
{
    const uint8_t *fp = ep->cap + frame * MFRAME_BYTES;
    pj_uint8_t ulaw[MFRAME_SAMPLES], pkt[256];
    const void *rtphdr = NULL;
    int hdrlen = 0, k;
    pj_ssize_t len;
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
    len = hdrlen + (int)sizeof(ulaw);
    return pj_sock_sendto(ep->tx, pkt, &len, 0, &ep->peer, sizeof(ep->peer));
}

/* Send the current RTCP report (SR + reception report + SDES CNAME). */
static void media_ep_send_rtcp(media_ep *ep)
{
    pj_uint8_t pkt[128];
    unsigned len = rtcp_build_report(&ep->rtcps, pkt, sizeof(pkt));
    pj_ssize_t slen = (pj_ssize_t)len;
    pj_sock_sendto(ep->rtcp, pkt, &slen, 0, &ep->peer_rtcp,
                   sizeof(ep->peer_rtcp));
}

static int media_ep_put(media_ep *ep, int tmo_ms)
{
    pj_uint8_t buf[256];
    pj_ssize_t len = (pj_ssize_t)sizeof(buf);
    const pjmedia_rtp_hdr *hdr = NULL;
    const void *payload = NULL;
    unsigned plen = 0;
    pjmedia_rtp_status seq_st;
    const pj_uint8_t *ul;
    pj_uint8_t pcm[MFRAME_SAMPLES * 2];
    pj_bool_t discarded = PJ_FALSE;
    pj_fd_set_t rfds;
    pj_time_val stmo;
    int k, seq;
    pj_status_t rc;

    PJ_FD_ZERO(&rfds);
    PJ_FD_SET(ep->rx, &rfds);
    stmo.sec = 0; stmo.msec = tmo_ms;
    if (pj_sock_select((int)ep->rx + 1, &rfds, NULL, NULL, &stmo) <= 0 ||
        !PJ_FD_ISSET(ep->rx, &rfds))
        return -1;
    rc = pj_sock_recvfrom(ep->rx, buf, &len, 0, NULL, NULL);
    if (rc != PJ_SUCCESS) return -1;
    rc = pjmedia_rtp_decode_rtp(&ep->rtp, buf, (int)len, &hdr, &payload, &plen);
    if (rc != PJ_SUCCESS) return -1;
    pjmedia_rtp_session_update(&ep->rtp, hdr, &seq_st);
    if (seq_st.status.flag.bad) return -1;

    seq = (int)pj_ntohs(hdr->seq);
    ul = (const pj_uint8_t*)payload;
    if (plen > MFRAME_SAMPLES) plen = MFRAME_SAMPLES;
    for (k = 0; k < (int)plen; ++k) {
        pj_int16_t s = (pj_int16_t)pjmedia_ulaw2linear(ul[k]);
        pcm[k * 2]     = (uint8_t)(s & 0xFF);
        pcm[k * 2 + 1] = (uint8_t)((s >> 8) & 0xFF);
    }
    if (ep->have_jb)
        pjmedia_jbuf_put_frame2(ep->jb, pcm, sizeof(pcm), 0, seq, &discarded);
    /* feed RTCP receive statistics (seq + RTP timestamp for jitter). */
    rtcp_rx_rtp(&ep->rtcps, (unsigned)seq, pj_ntohl(hdr->ts));
    return discarded ? 1 : 0;
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

/* ------------------------------------------------------------------ */
/* Real-time dual-thread media (stage 12).                             */
/*                                                                     */
/*   main task = sender : emits one 10 ms RTP frame per 10 ms (2 s)    */
/*   rx thread          : concurrently receives peer RTP -> jbuf       */
/*   play thread        : concurrently pulls one frame per 10 ms       */
/*                       from the jitter buffer into s_mplay          */
/*                                                                     */
/* Stats below are plain volatile counters (10 ms frames are atomic    */
/* enough here; no locks needed between the three threads).            */
/* ------------------------------------------------------------------ */
static media_ep g_ep;
static volatile int g_rx_done, g_play_done, g_sender_done;
static volatile int g_rx_got, g_rx_discard, g_rx_bad;
static volatile int g_play_normal, g_play_missing, g_play_zero;
static volatile int g_tx_ok;
static volatile long g_tx_ms, g_play_ms;

static void rx_thread(void *arg)
{
    int to = 0;
    PJ_UNUSED_ARG(arg);
    while (g_rx_got < MEDIA_FRAMES && to < 40) {
        int rc = media_ep_put(&g_ep, 50);   /* 50 ms poll */
        if (rc == 0) { g_rx_got++; to = 0; }
        else if (rc == 1) { g_rx_discard++; to = 0; }
        else { g_rx_bad++; to++; }          /* timeout */
    }
    g_rx_done = 1;
    vTaskDelete(NULL);
}

static void play_thread(void *arg)
{
    char ftype;
    pj_time_val t0, t1;
    int i;
    PJ_UNUSED_ARG(arg);
    pj_gettimeofday(&t0);
    for (i = 0; i < MEDIA_FRAMES; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));      /* 10 ms real-time pace */
        pjmedia_jbuf_get_frame(g_ep.jb, s_mplay + i * MFRAME_BYTES, &ftype);
        if (ftype == PJMEDIA_JB_NORMAL_FRAME) {
            g_play_normal++;
        } else if (ftype == PJMEDIA_JB_MISSING_FRAME) {
            g_play_missing++;
            memset(s_mplay + i * MFRAME_BYTES, 0, MFRAME_BYTES);
        } else {
            g_play_zero++;
            memset(s_mplay + i * MFRAME_BYTES, 0, MFRAME_BYTES);
        }
    }
    pj_gettimeofday(&t1);
    g_play_ms = (t1.sec - t0.sec) * 1000 + (t1.msec - t0.msec);
    g_play_done = 1;
    vTaskDelete(NULL);
}

static void sender_thread(void *arg)
{
    pj_time_val t0, t1;
    int i;
    PJ_UNUSED_ARG(arg);
    pj_gettimeofday(&t0);
    for (i = 0; i < MEDIA_FRAMES; ++i) {
        if (media_ep_send(&g_ep, i) == PJ_SUCCESS) {
            g_tx_ok++;
            rtcp_tx_rtp(&g_ep.rtcps, MFRAME_SAMPLES);
        }
        if ((i % 50) == 49)                 /* every 500 ms: SR */
            media_ep_send_rtcp(&g_ep);
        vTaskDelay(pdMS_TO_TICKS(10));      /* 10 ms real-time pace */
    }
    media_ep_send_rtcp(&g_ep);              /* final SR with full stats */
    pj_gettimeofday(&t1);
    g_tx_ms = (t1.sec - t0.sec) * 1000 + (t1.msec - t0.msec);
    g_sender_done = 1;
    vTaskDelete(NULL);
}

static int run_dual_media(pj_pool_t *pool)
{
    pj_str_t nm;
    int wait_ms;

    memset(&g_ep, 0, sizeof(g_ep));
    g_ep.rx = g_ep.tx = g_ep.rtcp = PJ_INVALID_SOCKET;

    printf("pj_sip_dual[%s]: REAL-TIME media rx=%u tx=%s:%u\r\n",
           ROLE_NAME, (unsigned)MY_RTP_PORT, GW_IP, (unsigned)PEER_RTP_PORT);

    if (media_ep_open(&g_ep, MY_RTP_PORT, GW_IP, PEER_RTP_PORT, MY_SSRC,
                      pool, pj_cstr(&nm, ROLE_NAME)) != PJ_SUCCESS) {
        printf("pj_sip_dual[%s]: media socket open failed\r\n", ROLE_NAME);
        return -1;
    }
    g_ep.cap = s_mcap;
    g_ep.play = s_mplay;

    if (!mic_capture(s_mcap, MEDIA_PCM_BYTES, MEDIA_RATE, MEDIA_FMT, 20000000UL)) {
        printf("pj_sip_dual[%s]: mic capture failed\r\n", ROLE_NAME);
        return -1;
    }
    printf("pj_sip_dual[%s]: media source peak=%ld\r\n", ROLE_NAME,
           media_peak(s_mcap, MEDIA_PCM_BYTES));

    /* Reset stats, then start the three real-time threads. */
    g_rx_done = g_play_done = g_sender_done = 0;
    g_rx_got = g_rx_discard = g_rx_bad = 0;
    g_play_normal = g_play_missing = g_play_zero = 0;
    g_tx_ok = 0;
    g_tx_ms = g_play_ms = 0;
    xTaskCreate(sender_thread, "pjdual-tx",  2048, NULL, 3, NULL);
    xTaskCreate(rx_thread,     "pjdual-rx",  2048, NULL, 3, NULL);

    /* Pre-fill: let rx_thread collect a small cushion before playback starts
     * so the opening frames aren't starved by network latency.  The sender
     * runs at the same priority as rx, so sending stays timely. */
    {
        int pf = 0;
        while (g_rx_got < 5 && pf < 250) { vTaskDelay(pdMS_TO_TICKS(2)); pf += 2; }
    }
    xTaskCreate(play_thread,   "pjdual-play", 2048, NULL, 2, NULL);

    /* The three threads run the 2 s call concurrently.  Wait for playback
     * to finish; while waiting, non-blocking drain the RTCP socket so we
     * parse the peer's SR/RR (loss/jitter/RTT). */
    while (!g_play_done) {
        pj_fd_set_t rfds;
        pj_time_val stmo;
        PJ_FD_ZERO(&rfds);
        PJ_FD_SET(g_ep.rtcp, &rfds);
        stmo.sec = 0; stmo.msec = 0;
        if (pj_sock_select((int)g_ep.rtcp + 1, &rfds, NULL, NULL, &stmo) > 0 &&
            PJ_FD_ISSET(g_ep.rtcp, &rfds)) {
            pj_uint8_t rbuf[256];
            pj_ssize_t rlen = (pj_ssize_t)sizeof(rbuf);
            if (pj_sock_recvfrom(g_ep.rtcp, rbuf, &rlen, 0, NULL, NULL) ==
                PJ_SUCCESS)
                rtcp_parse(&g_ep.rtcps, rbuf, (unsigned)rlen);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    wait_ms = 0;
    while (!g_rx_done && wait_ms < 3000) { vTaskDelay(pdMS_TO_TICKS(5)); wait_ms += 5; }

    /* Jitter-buffer quality report (before destroying it). */
    {
        pjmedia_jb_state jbs;
        pj_bzero(&jbs, sizeof(jbs));
        pjmedia_jbuf_get_state(g_ep.jb, &jbs);
        printf("pj_sip_dual[%s]:   jbuf size=%u prefetch=%u "
               "delay(avg/min/max/dev)=%u/%u/%u/%u ms lost=%u discard=%u "
               "empty=%u\r\n", ROLE_NAME, jbs.size, jbs.prefetch,
               jbs.avg_delay, jbs.min_delay, jbs.max_delay, jbs.dev_delay,
               jbs.lost, jbs.discard, jbs.empty);
    }
    pjmedia_jbuf_destroy(g_ep.jb);

    printf("pj_sip_dual[%s]: REAL-TIME media stats:\r\n", ROLE_NAME);
    printf("pj_sip_dual[%s]:   tx   %d/%d frames in %ld ms\r\n",
           ROLE_NAME, g_tx_ok, MEDIA_FRAMES, g_tx_ms);
    printf("pj_sip_dual[%s]:   rx   %d frames, discard=%d poll-timeout=%d\r\n",
           ROLE_NAME, g_rx_got, g_rx_discard, g_rx_bad);
    printf("pj_sip_dual[%s]:   play normal=%d missing=%d zero=%d in %ld ms, "
           "peak=%ld\r\n", ROLE_NAME, g_play_normal, g_play_missing,
           g_play_zero, g_play_ms, media_peak(s_mplay, MEDIA_PCM_BYTES));
    {
        pj_uint32_t exp = rtcp_expected(&g_ep.rtcps);
        pj_uint32_t got = g_ep.rtcps.rx.received;
        pj_uint32_t lost = (exp > got) ? exp - got : 0;
        pj_uint32_t frac = exp ? (lost * 100) / exp : 0;
        pj_uint32_t pfrac = (g_ep.rtcps.peer_fraction_lost * 100) / 256;
        printf("pj_sip_dual[%s]:   rtcp tx_pkt=%u tx_oct=%u | "
               "rx exp=%u got=%u lost=%u frac=%u%% jitter=%u | "
               "peer-lost=%u peer-frac=%u%% rtt=%u us\r\n",
               ROLE_NAME, g_ep.rtcps.tx.pkt_count, g_ep.rtcps.tx.octet_count,
               exp, got, lost, frac, g_ep.rtcps.rx.jitter >> 4,
               g_ep.rtcps.peer_cum_lost, pfrac,
               rtcp_rtt_us(&g_ep.rtcps));
    }

    media_pcm_to_u8(s_mplay, s_mplay_u8, MEDIA_PCM_BYTES);
    printf("pj_sip_dual[%s]: playing peer -> speaker\r\n", ROLE_NAME);
    audio_play(s_mplay_u8, MEDIA_PCM_BYTES / 2, MEDIA_RATE, AUDIO_FORMAT_U8);
    pj_thread_sleep(2500);
    audio_stop();

    if (g_ep.rx != PJ_INVALID_SOCKET) pj_sock_close(g_ep.rx);
    if (g_ep.tx != PJ_INVALID_SOCKET) pj_sock_close(g_ep.tx);
    if (g_ep.rtcp != PJ_INVALID_SOCKET) pj_sock_close(g_ep.rtcp);

    /* Real UDP path over slirp/hostfwd can drop ~5-10% of frames; accept
     * >= 85% delivery (a normal VoIP call stays usable at that level). */
    if (g_rx_got >= (MEDIA_FRAMES * 85) / 100 &&
        g_play_normal >= (MEDIA_FRAMES * 85) / 100)
        return 0;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Test entry.                                                         */
/* ------------------------------------------------------------------ */
int pj_sip_dual_test_run(void)
{
    pj_status_t rc;
    pj_pool_t *pool;
    pj_sockaddr_in local;
    pj_sockaddr host;
    pjsip_ua_init_param ua_prm;
    pjsip_inv_callback inv_cb;
    pjsip_host_port pub;
    pj_time_val tmo;
    int i, pass = 0;
    char pub_ip[32];

    printf("\r\n=== PJSIP DUAL QEMU call test [%s] ===\r\n", ROLE_NAME);

    rc = pj_init();
    if (rc != PJ_SUCCESS) { printf("pj_sip_dual[%s]: pj_init failed (%d)\r\n", ROLE_NAME, rc); return -1; }
    pj_caching_pool_init(&g_cp, &pj_pool_factory_default_policy, 0);

    rc = pjsip_endpt_create(&g_cp.factory, "mps2-an505-dual", &g_endpt);
    if (rc != PJ_SUCCESS) { printf("pj_sip_dual[%s]: endpt failed (%d)\r\n", ROLE_NAME, rc); return -1; }

    rc = pjsip_tsx_layer_init_module(g_endpt);
    if (rc != PJ_SUCCESS) { printf("pj_sip_dual[%s]: tsx failed (%d)\r\n", ROLE_NAME, rc); return -1; }

    pj_bzero(&ua_prm, sizeof(ua_prm));
    rc = pjsip_ua_init_module(g_endpt, &ua_prm);
    if (rc != PJ_SUCCESS) { printf("pj_sip_dual[%s]: ua failed (%d)\r\n", ROLE_NAME, rc); return -1; }
    g_ua = pjsip_ua_instance();

    pj_bzero(&inv_cb, sizeof(inv_cb));
    inv_cb.on_state_changed = &inv_on_state_changed;
    rc = pjsip_inv_usage_init(g_endpt, &inv_cb);
    if (rc != PJ_SUCCESS) { printf("pj_sip_dual[%s]: inv_usage failed (%d)\r\n", ROLE_NAME, rc); return -1; }
    rc = pjsip_100rel_init_module(g_endpt);
    if (rc != PJ_SUCCESS) { printf("pj_sip_dual[%s]: 100rel failed (%d)\r\n", ROLE_NAME, rc); return -1; }
    rc = pjsip_timer_init_module(g_endpt);
    if (rc != PJ_SUCCESS) { printf("pj_sip_dual[%s]: timer failed (%d)\r\n", ROLE_NAME, rc); return -1; }
#if defined(PJ_DUAL_ROLE_CALLEE)
    rc = pjsip_endpt_register_module(g_endpt, &mod_dual_uas);
    if (rc != PJ_SUCCESS) { printf("pj_sip_dual[%s]: uas mod failed (%d)\r\n", ROLE_NAME, rc); return -1; }
#endif

    /* UDP transport: bind guest SIP_PORT, publish the host-forwarded
     * address so Via/Contact point at 10.0.2.2:<ext> for the peer. */
    rc = pj_gethostip(pj_AF_INET(), &host);
    if (rc != PJ_SUCCESS) { printf("pj_sip_dual[%s]: gethostip failed (%d)\r\n", ROLE_NAME, rc); return -1; }
    pj_sockaddr_print(&host, g_local_ip, sizeof(g_local_ip), 0);
    printf("pj_sip_dual[%s]: local IP = %s\r\n", ROLE_NAME, g_local_ip);

    pj_sockaddr_in_init(&local, NULL, (pj_uint16_t)SIP_PORT);
    local.sin_addr = host.ipv4.sin_addr;
    pj_ansi_snprintf(pub_ip, sizeof(pub_ip), "%s:%d", GW_IP, MY_EXT_SIP);
    SET_STR(pub.host, "");
    pj_strset(&pub.host, GW_IP, (pj_ssize_t)pj_ansi_strlen(GW_IP));
    pub.port = (pj_uint16_t)MY_EXT_SIP;
    rc = pjsip_udp_transport_start(g_endpt, &local, &pub, 1, &g_tp);
    if (rc != PJ_SUCCESS) { printf("pj_sip_dual[%s]: udp failed (%d)\r\n", ROLE_NAME, rc); return -1; }
    printf("pj_sip_dual[%s]: UDP transport up guest=%s:%d published=%s\r\n",
           ROLE_NAME, g_local_ip, SIP_PORT, pub_ip);

    /* SDP with the host-forwarded IP + our RTP port. */
    pool = pj_pool_create(&g_cp.factory, "sdp", 1024, 1024, NULL);
    g_sdp = create_audio_sdp(pool, GW_IP, MY_RTP_PORT);
    if (!g_sdp) { printf("pj_sip_dual[%s]: create_audio_sdp failed\r\n", ROLE_NAME); return -1; }

    g_confirmed = 0;
    g_failed = 0;

#if defined(PJ_DUAL_ROLE_CALLER)
    printf("pj_sip_dual[caller]: dialing %s:%d\r\n", GW_IP, CALLEE_EXT_SIP);
    rc = uac_dial();
    if (rc != PJ_SUCCESS) { printf("pj_sip_dual[caller]: dial failed (%d)\r\n", rc); return -1; }
#else
    printf("pj_sip_dual[callee]: waiting for INVITE...\r\n");
#endif

    /* Event loop until CONFIRMED (or timeout). */
    tmo.sec = 0;
    tmo.msec = 100;
    for (i = 0; i < MAX_LOOP && !g_confirmed && !g_failed; ++i) {
        pjsip_endpt_handle_events(g_endpt, &tmo);
        if ((i & 15) == 0)
            printf("pj_sip_dual[%s]: waiting... %d\r\n", ROLE_NAME, i);
    }

    if (g_confirmed && !g_failed) {
        printf("pj_sip_dual[%s]: INVITE CONFIRMED\r\n", ROLE_NAME);
        if (run_dual_media(pool) == 0) {
            printf("pj_sip_dual[%s]: media ALL PASSED\r\n", ROLE_NAME);
            pass = 1;
        } else {
            printf("pj_sip_dual[%s]: media FAILED\r\n", ROLE_NAME);
        }
    } else {
        printf("pj_sip_dual[%s]: FAILED (confirmed=%d failed=%d)\r\n",
               ROLE_NAME, g_confirmed, g_failed);
    }

    if (g_inv) pjsip_inv_terminate(g_inv, 0, PJ_FALSE);
    pjsip_endpt_destroy(g_endpt);
    pj_caching_pool_destroy(&g_cp);
    pj_shutdown();

    printf("pj_sip_dual[%s]: %s\r\n", ROLE_NAME, pass ? "ALL PASSED" : "FAILED");
    return pass ? 0 : -1;
}
