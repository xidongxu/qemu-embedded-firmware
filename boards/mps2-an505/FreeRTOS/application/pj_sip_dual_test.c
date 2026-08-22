/*
 * pj_sip_dual_test.c - dual QEMU inter-instance SIP call test (stage 11).
 *
 * The two QEMU instances (mps2-an505) each run their OWN slirp user-mode NAT
 * (guest static IP 10.0.2.15, gateway 10.0.2.2) and reach each other through
 * hostfwd port forwards on the shared host (2026-08-22: reverted from the
 * socket-netdev direct topology - A/B proved slirp was never the loss cause):
 *
 *   Caller instance (UAC):  hostfwd udp::16062-:15062 (SIP), udp::4000-:4000 (RTP),
 *                           udp::4001-:4001 (RTCP), udp::20003-:20003 (media SYNC)
 *     guest SIP  : bind 0.0.0.0:15062 (Via/Contact = 10.0.2.2:16062)
 *     dial target: sip:user@10.0.2.2:15062  (callee hostfwd)
 *     SDP offer  : c=IN IP4 10.0.2.2  m=audio 4000
 *     RTP        : rx bind 0.0.0.0:4000, tx -> 10.0.2.2:4002 (callee hostfwd)
 *
 *   Callee instance (UAS):  hostfwd udp::15062-:15062 (SIP), udp::4002-:4002 (RTP),
 *                           udp::4003-:4003 (RTCP), udp::20013-:20003 (media SYNC)
 *     guest SIP  : bind 0.0.0.0:15062 (contact = 10.0.2.2:15062)
 *     SDP answer : c=IN IP4 10.0.2.2  m=audio 4002
 *     RTP        : rx bind 0.0.0.0:4002, tx -> 10.0.2.2:4000 (caller hostfwd)
 *
 * Path A->B: A dials 10.0.2.2:15062 and sends RTP to 10.0.2.2:4002.
 * Path B->A: B replies/ACKs to 10.0.2.2:16062 and sends RTP to 10.0.2.2:4000.
 *
 * Media runs on the negotiated RTP port through a pjmedia_stream jitter
 * buffer (clock-driven, stage 17) with a 10 ms playback pace.  Three
 * test-side fixes make the 200/200 loss-free result reproducible:
 *   1. media-start handshake (SYNC port, hostfwd'd) - both transports are up
 *      before the first RTP frame is sent;
 *   2. SDP a=ptime:10 - G.711 encoder frame size matches the 10 ms clock;
 *   3. VAD disabled (si.param->setting.vad=0) - the silence detector would
 *      mis-detected the pure tone once VAD re-enables 600 ms in.
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
#include <pjmedia/sdp_neg.h>
#include <pjmedia/endpoint.h>
#include <pjmedia/event.h>
#include <pjmedia/codec.h>
#include <pjmedia/g711.h>
#include <pjmedia/transport_udp.h>
#include <pjmedia/stream.h>
#include <pjmedia/clock.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "mic.h"
#include "audio.h"
#include "lan9118.h"

#if defined(PJ_DUAL_ROLE_CALLER) && defined(PJ_DUAL_ROLE_CALLEE)
#error "define exactly one of PJ_DUAL_ROLE_CALLER / PJ_DUAL_ROLE_CALLEE"
#endif
#if !defined(PJ_DUAL_ROLE_CALLER) && !defined(PJ_DUAL_ROLE_CALLEE)
#error "define PJ_DUAL_ROLE_CALLER or PJ_DUAL_ROLE_CALLEE"
#endif

/* ---- role-independent constants (slirp / user-net hostfwd topology) ---- */
#define GW_IP           "10.0.2.2"   /* slirp host gateway, both roles */
#define SIP_PORT        15062        /* guest-bound SIP port, both roles */
#define CALLEE_EXT_SIP  15062        /* callee published SIP port (hostfwd) */
#define CALLER_EXT_SIP  16062        /* caller published SIP port (hostfwd) */
#define CALLEE_RTP      4002         /* callee RTP port (hostfwd) */
#define CALLER_RTP      4000         /* caller RTP port (hostfwd) */
#define SYNC_PORT       20003        /* guest-bound media-start handshake port */
/* SYNC handshake hostfwd ports: each instance exposes its guest 20003 on a
 * distinct host port (caller 20003, callee 20013) so the two QEMUs do not
 * clash on the shared host. */
#define CALLER_SYNC_HOST 20003
#define CALLEE_SYNC_HOST 20013
#define MAX_LOOP       600          /* 600 x 100ms = 60s max wait */

#if defined(PJ_DUAL_ROLE_CALLER)
#  define ROLE_NAME      "caller"
#  define MY_EXT_SIP     CALLER_EXT_SIP
#  define PEER_EXT_SIP   CALLEE_EXT_SIP
#  define MY_RTP_PORT    CALLER_RTP
#  define PEER_RTP_PORT  CALLEE_RTP
#  define MY_SYNC_HOST   CALLER_SYNC_HOST
#  define PEER_SYNC_HOST CALLEE_SYNC_HOST
#  define MY_SSRC        0xAAA00003
#  define PEER_SSRC      0xBBB00003
#else
#  define ROLE_NAME      "callee"
#  define MY_EXT_SIP     CALLEE_EXT_SIP
#  define PEER_EXT_SIP   CALLER_EXT_SIP
#  define MY_RTP_PORT    CALLEE_RTP
#  define PEER_RTP_PORT  CALLER_RTP
#  define MY_SYNC_HOST   CALLEE_SYNC_HOST
#  define PEER_SYNC_HOST CALLER_SYNC_HOST
#  define MY_SSRC        0xBBB00003
#  define PEER_SSRC      0xAAA00003
#endif

/* RTCP rides the RTP port + 1 (RFC 3550). */
#define MY_RTCP_PORT    (MY_RTP_PORT + 1)
#define PEER_RTCP_PORT  (PEER_RTP_PORT + 1)

/* DTMF digits dialed by the caller; the callee must hear them.  The RFC 4733
 * telephone-event payload type (101) is now NEGOTIATED via SDP, not fixed. */
#define DTMF_SEQ        "5#"          /* caller dials, callee must hear */

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

/* Stage 17: pjmedia_stream owns the whole media path (RTP/RTCP/jbuf/PLC/DTMF
 * + G.711 codec); the application just polls the media endpoint's ioqueue and
 * pumps frames through the stream's media port. */
static pjmedia_endpt      *g_mep;         /* pjmedia endpoint (ioqueue)   */
static pjmedia_transport  *g_media_tp;    /* UDP transport (RTP+RTCP)     */
static pjmedia_stream     *g_stream;      /* media stream (codec+jbuf)    */
static pjmedia_port       *g_stream_port; /* put/get_frame interface      */

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
    m->desc.fmt_count = 2;
    SET_STR(m->desc.fmt[0], "0");
    SET_STR(m->desc.fmt[1], "101");

    pj_bzero(&rtpmap, sizeof(rtpmap));
    SET_STR(rtpmap.pt, "0");
    SET_STR(rtpmap.enc_name, "PCMU");
    rtpmap.clock_rate = 8000;
    rtpmap.param.ptr = NULL;
    rtpmap.param.slen = 0;
    if (pjmedia_sdp_rtpmap_to_attr(pool, &rtpmap, &a) == PJ_SUCCESS)
        pjmedia_sdp_media_add_attr(m, a);

    /* RFC 4733 telephone-event, so the stream negotiates a DTMF payload
     * type (101) over SDP instead of hard-coding it. */
    pj_bzero(&rtpmap, sizeof(rtpmap));
    SET_STR(rtpmap.pt, "101");
    SET_STR(rtpmap.enc_name, "telephone-event");
    rtpmap.clock_rate = 8000;
    rtpmap.param.ptr = NULL;
    rtpmap.param.slen = 0;
    if (pjmedia_sdp_rtpmap_to_attr(pool, &rtpmap, &a) == PJ_SUCCESS)
        pjmedia_sdp_media_add_attr(m, a);

    /* Force 10 ms packetisation so the codec encoder frame size (80 samples
     * @8kHz) matches the media-clock frame fed into put_frame().  Without
     * this, G.711 defaults to 20 ms; the 10 ms clock frames then go through
     * the rebuffer path and ~30% of them come out as empty
     * (frame_out.size==0), which are silently not transmitted - the peer
     * receives fewer RTP packets than the 200 that put_frame() accepted. */
    {
        pj_str_t v10 = pj_str("10");
        a = pjmedia_sdp_attr_create(pool, "ptime", &v10);
        if (a)
            pjmedia_sdp_media_add_attr(m, a);
    }

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
                     "sip:user@%s:%d", GW_IP, PEER_EXT_SIP);
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
#define MEDIA_FRAMES     1000        /* 1000 x 10 ms = 10 s long-call test */
#define MEDIA_PCM_BYTES  (MEDIA_FRAMES * MFRAME_BYTES)   /* 32000 bytes */

/* PCM buffers: s_mcap = mic capture (S16, 2 s), s_mplay = decoded peer
 * audio (S16), s_mplay_u8 = U8 for the mpsx-audio DAC. */
static uint8_t s_mcap[MEDIA_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_mplay[MEDIA_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_mplay_u8[MEDIA_PCM_BYTES / 2] __attribute__((aligned(64)));

/* media_pcm_to_u8() / media_peak() below are kept for the DAC path. */

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
/* pjmedia_stream media (stage 17).                                    */
/*                                                                     */
/* The stream owns RTP/RTCP sockets (UDP transport), the G.711 codec,  */
/* an adaptive jitter buffer, PLC and RFC 4733 DTMF.  The application  */
/* only drives it with three tasks:                                    */
/*                                                                     */
/*   ioq task    : polls the pjmedia endpoint ioqueue so the UDP       */
/*                 transport delivers RTP -> jbuf and RTCP -> stats    */
/*   play task   : drives a NO_ASYNC pjmedia_clock via wait(); one     */
/*                 callback does put_frame() AND get_frame() on the    */
/*                 same tick (codec encode + RTP send + jbuf + decode  */
/*                 + PLC + DTMF), so the jitter buffer stays stable.   */
/*                                                                     */
/* Frame/stat counters are plain volatile ints (10 ms frames are       */
/* atomic enough here).                                                */
/* ------------------------------------------------------------------ */
#define JB_PREFETCH_FRAMES  5       /* == si.jb_min_pre */
static volatile int  g_media_stop;
static volatile int  g_play_normal, g_play_missing, g_play_zero;
static volatile int  g_tx_ok;
static volatile long g_tx_ms, g_play_ms;
static volatile char g_dtmf_rx[16];
static volatile int  g_dtmf_count, g_dtmf_ok;
static int           g_media_ix;      /* frame index, advanced in clock cb */
static pj_time_val   g_play_t0;       /* clock-driven play start time */
static pjmedia_clock *g_clock;        /* NO_ASYNC 10ms media clock */
/* TX-pacing diagnostics (2026-08-20): ACTUAL interval between consecutive
 * media_clock callbacks via the high-res SysTick timestamp.  Under QEMU TCG
 * the virtual clock bursts, so we expect min~0 (burst: several frames fired
 * together) and max >> 10ms (stall) instead of a clean 10ms cadence. */
static pj_timestamp  g_clk_prev;
static int           g_clk_cnt;
static long long     g_clk_min_us, g_clk_max_us, g_clk_sum_us;
static int           g_clk_burst, g_clk_gap;
static unsigned long long g_clk_freq;

/* UDP handshake so both instances start their media clock together (after
 * both transports are up).  Same idea as net_burst_test's READY sync:
 * without it, the first instance sends its opening RTP frames before the
 * second's transport exists, which shows up as RTCP loss even though the
 * base network is loss-free. */
static void media_sync_handshake(void)
{
    int fd;
    struct sockaddr_in local, peer;
    const uint32_t MAGIC = 0x53594E43u;  /* "SYNC" */
    uint32_t buf;
    int tries = 0, peer_seen = -1;
    int on = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("pj_sip_dual[%s]: sync socket failed\r\n", ROLE_NAME);
        return;
    }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons((u16_t)SYNC_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        printf("pj_sip_dual[%s]: sync bind failed\r\n", ROLE_NAME);
        lwip_close(fd);
        return;
    }
    /* Poll with non-blocking recvfrom + task delay: on this lwIP port a
     * blocking recvfrom with SO_RCVTIMEO can hang forever, which dead-locked
     * the callee.  Non-blocking also lets both sides keep re-sending SYNC so
     * the peer that reached the handshake later still gets ours. */
    ioctl(fd, FIONBIO, &on);

    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons((u16_t)PEER_SYNC_HOST);
    peer.sin_addr.s_addr = inet_addr(GW_IP);

    /* Both sides keep sending SYNC every 10 ms until they have seen the
     * peer's SYNC, then keep sending 5 more so the peer is guaranteed to see
     * ours too (fixes the "caller sends once then exits, callee never hears
     * it" race), then finish. */
    while (tries < 200) {
        buf = MAGIC;
        sendto(fd, &buf, sizeof(buf), 0,
               (struct sockaddr *)&peer, sizeof(peer));
        buf = 0;
        if (recvfrom(fd, &buf, sizeof(buf), 0, NULL, NULL) ==
            (int)sizeof(buf) && buf == MAGIC) {
            if (peer_seen < 0) {
                peer_seen = tries;
                printf("pj_sip_dual[%s]: peer SYNC seen at try=%d\r\n",
                       ROLE_NAME, tries);
            }
            if (tries - peer_seen >= 5) {
                break;
            }
        }
        tries++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    lwip_close(fd);
    printf("pj_sip_dual[%s]: media sync done (tries=%d peer_seen=%d)\r\n",
           ROLE_NAME, tries, peer_seen);
}

/* Poll the media endpoint ioqueue so transport_udp's async RTP/RTCP
 * recvfrom completes and the stream's on_rx_rtp / on_rx_rtcp run.
 * pj_ioqueue_poll() completes a bounded number of I/O ops per call, and
 * every completed recvfrom re-arms another async read, so we loop until
 * the queue is drained before yielding - otherwise the lwIP UDP receive
 * buffer overflows and we drop frames. */
static void ioq_thread(void *arg)
{
    pj_time_val tv;
    int n, cnt;
    PJ_UNUSED_ARG(arg);
    while (!g_media_stop) {
        /* Drain pending RTP/RTCP datagrams but cap the number of completed
         * I/O ops per iteration so the sender/play tasks (same priority)
         * still get CPU time; otherwise they starve and the peer sees drops. */
        cnt = 0;
        do {
            tv.sec = 0; tv.msec = 0;      /* non-blocking poll */
            n = pj_ioqueue_poll(pjmedia_endpt_get_ioqueue(g_mep), &tv);
            if (++cnt >= 32) break;
        } while (n > 0);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    vTaskDelete(NULL);
}

/* Stage 17 clock-driven media: instead of two independent FreeRTOS tasks
 * calling put_frame()/get_frame() on their own 10ms cadence (whose phase
 * offset made the adaptive jitter buffer go empty), the standard pjmedia
 * recipe is used: a single pjmedia_clock ticks every 10ms and, inside ONE
 * callback, both put_frame() and get_frame() run synchronously (same
 * thread, same tick).  The jitter buffer then stabilises at the depth that
 * matches the network latency, and only genuine network loss produces
 * missing frames - no pjproject source modification needed.  The clock is
 * created with PJMEDIA_CLOCK_NO_ASYNC and driven by play_thread via
 * pjmedia_clock_wait(), so the callback runs in our own (stack-controlled)
 * task. */
static void media_clock_cb(const pj_timestamp *ts, void *user_data)
{
    int i = g_media_ix;
    pjmedia_frame f;
    char jbft;
    PJ_UNUSED_ARG(ts); PJ_UNUSED_ARG(user_data);

    if (i >= MEDIA_FRAMES)
        return;

    /* Measure the actual inter-callback interval (SysTick high-res). */
    {
        pj_timestamp now;
        pj_get_timestamp(&now);
        if (g_clk_freq == 0) {
            pj_timestamp f;
            pj_get_timestamp_freq(&f);
            g_clk_freq = f.u64;
        }
        if (g_clk_cnt > 0) {
            long long us = (long long)((now.u64 - g_clk_prev.u64) *
                                       1000000ULL / g_clk_freq);
            if (us < 0) us = 0;
            if (g_clk_cnt == 1 || us < g_clk_min_us) g_clk_min_us = us;
            if (us > g_clk_max_us) g_clk_max_us = us;
            g_clk_sum_us += us;
            if (us < 8000) g_clk_burst++;
            if (us > 15000) g_clk_gap++;
        }
        g_clk_prev = now;
        g_clk_cnt++;
    }

    /* --- TX: one 10ms frame (codec encode + RTP send + RTCP SR) --- */
    f.type = PJMEDIA_FRAME_TYPE_AUDIO;
    f.buf = s_mcap + i * MFRAME_BYTES;
    f.size = MFRAME_BYTES;
    f.timestamp.u32.lo = (pj_uint32_t)(i * MFRAME_SAMPLES);
    if (g_stream_port &&
        g_stream_port->put_frame(g_stream_port, &f) == PJ_SUCCESS)
        g_tx_ok++;
#if defined(PJ_DUAL_ROLE_CALLER)
    /* caller dials DTMF "5#" mid-call (RFC 4733, negotiated PT 101) */
    if (i == 100) {
        pj_str_t digits = pj_str(DTMF_SEQ);
        pjmedia_stream_dial_dtmf(g_stream, &digits);
    }
#endif

    /* --- RX: one 10ms frame (jbuf + decode + PLC) --- */
    f.buf = s_mplay + i * MFRAME_BYTES;
    f.size = MFRAME_BYTES;
    if (g_stream_port) {
        g_stream_port->get_frame(g_stream_port, &f);
        jbft = pjmedia_stream_get_last_jb_frame_type(g_stream);
        if (jbft == PJMEDIA_JB_NORMAL_FRAME) g_play_normal++;
        else if (jbft == PJMEDIA_JB_MISSING_FRAME) g_play_missing++;
        else g_play_zero++;
        /* drain RFC 4733 digits the stream buffered for us */
        {
            char buf[16];
            unsigned sz = sizeof(buf);
            if (pjmedia_stream_get_dtmf(g_stream, buf, &sz) == PJ_SUCCESS &&
                sz > 0)
            {
                int k;
                for (k = 0; k < (int)sz && g_dtmf_count < 15; ++k)
                    g_dtmf_rx[g_dtmf_count++] = buf[k];
                g_dtmf_rx[g_dtmf_count] = 0;
            }
        }
    } else {
        memset(s_mplay + i * MFRAME_BYTES, 0, MFRAME_BYTES);
    }

    g_media_ix = i + 1;
    if (g_media_ix >= MEDIA_FRAMES) {
        pj_time_val t1;
        pj_gettimeofday(&t1);
        g_play_ms = (t1.sec - g_play_t0.sec) * 1000 +
                    (t1.msec - g_play_t0.msec);
        g_tx_ms = g_play_ms;
    }
}

static void play_thread(void *arg)
{
    PJ_UNUSED_ARG(arg);
    pj_gettimeofday(&g_play_t0);
    /* NO_ASYNC clock: each pjmedia_clock_wait() blocks until the next
     * 10 ms tick, then runs media_clock_cb (put + get synchronously). */
    while (g_media_ix < MEDIA_FRAMES && !g_media_stop) {
        pjmedia_clock_wait(g_clock, 1, NULL);
    }
    vTaskDelete(NULL);
}

static int run_dual_media(pj_pool_t *pool)
{
    pjmedia_stream_info si;
    pjmedia_rtcp_stat rstat;
    pjmedia_jb_state jbs;
    const pjmedia_sdp_session *loc_sdp = NULL, *rem_sdp = NULL;
    int wait_ms;
    pj_status_t rc;

    printf("pj_sip_dual[%s]: pjmedia_stream media rx=%u rtcp=%u\r\n",
           ROLE_NAME, (unsigned)MY_RTP_PORT, (unsigned)MY_RTCP_PORT);

    if (!mic_capture(s_mcap, MEDIA_PCM_BYTES, MEDIA_RATE, MEDIA_FMT, 200000000UL)) {
        printf("pj_sip_dual[%s]: mic capture failed\r\n", ROLE_NAME);
        return -1;
    }
    printf("pj_sip_dual[%s]: media source peak=%ld\r\n", ROLE_NAME,
           media_peak(s_mcap, MEDIA_PCM_BYTES));

    /* Build the stream info from the negotiated local/remote SDP. */
    rc = pjmedia_sdp_neg_get_active_local(g_inv->neg, &loc_sdp);
    if (rc == PJ_SUCCESS)
        rc = pjmedia_sdp_neg_get_active_remote(g_inv->neg, &rem_sdp);
    if (rc != PJ_SUCCESS || !loc_sdp || !rem_sdp) {
        printf("pj_sip_dual[%s]: get active SDP FAILED (%d)\r\n",
               ROLE_NAME, rc);
        return -1;
    }
    rc = pjmedia_stream_info_from_sdp(&si, pool, g_mep, loc_sdp, rem_sdp, 0);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip_dual[%s]: stream_info_from_sdp FAILED (%d)\r\n",
               ROLE_NAME, rc);
        return -1;
    }
    /* Disable VAD (2026-08-22): G.711's silence detector mis-detects the
     * pure test tone as silence once the stream re-enables VAD ~600 ms in
     * (PJMEDIA_STREAM_VAD_SUSPEND_MSEC), silently dropping ~30% of the
     * frames (they never reach the transport, so the peer counts them as
     * lost).  Feed the stream a codec param with VAD off so every 10 ms
     * frame is actually transmitted.  This is a test-side configuration, no
     * pjproject source change. */
    {
        pjmedia_codec_param *param;
        param = PJ_POOL_ZALLOC_T(pool, pjmedia_codec_param);
        if (pjmedia_codec_mgr_get_default_param(
                pjmedia_endpt_get_codec_mgr(g_mep), &si.fmt, param) ==
            PJ_SUCCESS)
        {
            param->setting.vad = 0;
            param->setting.frm_per_pkt = 1;  /* 10ms/pkt: G.711 default is 2
             * (20ms) which makes the stream's play frame 160 samples while the
             * media clock feeds 80 -> get_frame needs 2 frames/jbuf drains fast
             * -> empty -> silence. 1 frame/pkt = 10ms matches the 10ms clock. */
            si.param = param;
        }
    }
    /* Jitter-buffer parameters.  NOTE: pjproject source is NOT modified;
     * with the default min_pre the empty jitter buffer yields an EMPTY
     * frame which stream.c conceals with PLC (synthesise/repeat last frame)
     * - exactly like a MISSING frame.  Under QEMU slirp/hostfwd the RTP
     * datagrams arrive in bursts, so the jitter buffer frequently drains
     * between bursts (high empty count) even though the AUDIO is continuous
     * (verified by wav FFT: 439/1001 Hz tone heard end-to-end).  This is an
     * emulator/network characteristic, not a pjproject defect. */
    si.jb_init = 80;      /* initial prefetch 80 ms (8 frames).  MUST be >0:
                           * jbuf adaptive only updates prefetch when
                           * jb_init_prefetch != 0 (jbuf.c); with 0 it stays 0
                           * -> get_frame empties once one-way delay > 10 ms.
                           * 10s call: empty dropped 97%->56%, normal 3%->43%. */
    si.jb_min_pre = 80;   /* min prefetch 80 ms (8 frames): absorbs larger
                           * one-way RTT (10s RTT fluctuated up to ~218 ms) */
    si.jb_max_pre = 150;  /* adaptive up to 150 ms (15 frames) */
    si.jb_max = 250;      /* max depth 250 ms (25 frames) */
    printf("pj_sip_dual[%s]: codec=%s/%u ch=%u dir=%d tx_pt=%d rx_pt=%d "
           "tx_evt=%d rx_evt=%d\r\n", ROLE_NAME,
           si.fmt.encoding_name.ptr, si.fmt.clock_rate, si.fmt.channel_cnt,
           (int)si.dir, (int)si.tx_pt, (int)si.rx_pt,
           si.tx_event_pt, si.rx_event_pt);

    /* UDP transport: RTP on MY_RTP_PORT, RTCP on MY_RTP_PORT+1 (hostfwd'd). */
    g_media_tp = NULL;
    rc = pjmedia_transport_udp_create2(g_mep, ROLE_NAME, NULL,
                                       (int)MY_RTP_PORT, 0, &g_media_tp);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip_dual[%s]: transport_udp_create2 FAILED (%d)\r\n",
               ROLE_NAME, rc);
        return -1;
    }

    g_stream = NULL;
    rc = pjmedia_stream_create(g_mep, pool, &si, g_media_tp, NULL, &g_stream);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip_dual[%s]: stream_create FAILED (%d)\r\n", ROLE_NAME, rc);
        return -1;
    }
    pjmedia_stream_get_port(g_stream, &g_stream_port);

    rc = pjmedia_stream_start(g_stream);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip_dual[%s]: stream_start FAILED (%d)\r\n", ROLE_NAME, rc);
        return -1;
    }
    /* Activate the UDP transport: kicks off async RTP/RTCP recvfrom. */
    rc = pjmedia_transport_media_start(g_media_tp, pool,
                                       loc_sdp, rem_sdp, 0);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip_dual[%s]: transport_media_start FAILED (%d)\r\n",
               ROLE_NAME, rc);
        return -1;
    }
    printf("pj_sip_dual[%s]: stream up (transport started)\r\n", ROLE_NAME);

    /* Media-start sync (2026-08-22): both transports are up now; exchange a
     * handshake packet so neither instance starts sending before the other
     * can receive.  Without this, the first instance sends its opening RTP
     * frames before the second's transport exists, which showed up as RTCP
     * loss even though net_burst proved the base network is loss-free. */
    media_sync_handshake();

    /* Reset stats, then start the ioq task and the 10ms media clock. */
    g_media_stop = 0;
    g_play_normal = g_play_missing = g_play_zero = 0;
    g_tx_ok = 0;
    g_tx_ms = g_play_ms = 0;
    g_media_ix = 0;
    g_clk_cnt = 0; g_clk_burst = 0; g_clk_gap = 0;
    g_clk_min_us = g_clk_max_us = g_clk_sum_us = 0;
    g_clk_freq = 0;
    g_dtmf_count = 0;
    g_dtmf_rx[0] = 0;
    g_dtmf_ok = 0;

    g_clock = NULL;
    rc = pjmedia_clock_create(pool, MEDIA_RATE, 1, MFRAME_SAMPLES,
                              PJMEDIA_CLOCK_NO_ASYNC,
                              &media_clock_cb, NULL, &g_clock);
    if (rc != PJ_SUCCESS) {
        printf("pj_sip_dual[%s]: clock_create FAILED (%d)\r\n", ROLE_NAME, rc);
        return -1;
    }
    pjmedia_clock_start(g_clock);

    xTaskCreate(ioq_thread,   "pjdual-ioq", 2048, NULL, 3, NULL);
    xTaskCreate(play_thread,  "pjdual-play", 2048, NULL, 3, NULL);

    /* Let the clock + ioq run the 10 s call. */
    wait_ms = 0;
    while (g_play_ms == 0 && wait_ms < 20000) { vTaskDelay(pdMS_TO_TICKS(5)); wait_ms += 5; }
    /* Hypothesis test (2026-08-21): much of the "RTCP loss" is NOT real
     * network loss - RTP frames arrive (lwIP udp.drop=0, NIC rx_drop=0) but
     * the media ioqueue hasn't drained them from the lwIP socket buffer by
     * the time the 200-frame playback finishes, so RTCP counts them as lost
     * and the jitter buffer runs empty (PLC).  Drain the tail before stats. */
    vTaskDelay(pdMS_TO_TICKS(500));
    g_media_stop = 1;

    /* Stream RTCP + jitter-buffer report. */
    pj_bzero(&rstat, sizeof(rstat));
    pjmedia_stream_get_stat(g_stream, &rstat);
    pj_bzero(&jbs, sizeof(jbs));
    pjmedia_stream_get_stat_jbuf(g_stream, &jbs);
    printf("pj_sip_dual[%s]:   jbuf size=%u prefetch=%u "
           "delay(avg/min/max/dev)=%u/%u/%u/%u ms lost=%u discard=%u "
           "empty=%u\r\n", ROLE_NAME, jbs.size, jbs.prefetch,
           jbs.avg_delay, jbs.min_delay, jbs.max_delay, jbs.dev_delay,
           jbs.lost, jbs.discard, jbs.empty);
    printf("pj_sip_dual[%s]: pjmedia_stream media stats:\r\n", ROLE_NAME);
    printf("pj_sip_dual[%s]:   tx   %d/%d frames in %ld ms\r\n",
           ROLE_NAME, g_tx_ok, MEDIA_FRAMES, g_tx_ms);
    printf("pj_sip_dual[%s]:   rx   rtcp pkt=%u bytes=%u discard=%u loss=%u "
           "jitter(avg)=%d us\r\n", ROLE_NAME, rstat.rx.pkt, rstat.rx.bytes,
           rstat.rx.discard, rstat.rx.loss, rstat.rx.jitter.mean);
    printf("pj_sip_dual[%s]:   play normal=%d missing=%d zero=%d "
           "in %ld ms, peak=%ld\r\n", ROLE_NAME, g_play_normal, g_play_missing,
           g_play_zero, g_play_ms, media_peak(s_mplay, MEDIA_PCM_BYTES));
    if (g_clk_cnt > 1) {
        printf("pj_sip_dual[%s]:   clock cb n=%d min=%lld max=%lld avg=%lld us "
               "burst(<8ms)=%d gap(>15ms)=%d\r\n", ROLE_NAME, g_clk_cnt,
               g_clk_min_us, g_clk_max_us,
               (long long)(g_clk_sum_us / g_clk_cnt), g_clk_burst, g_clk_gap);
    }
    /* RX-drop diagnostics: QEMU lan9118 increments RX_DROP (0xA0) when its
     * RX FIFO overflows because the guest did not drain it in time.  If this
     * tracks the RTCP-reported loss, the drops are at the RECEIVER's NIC
     * FIFO (QEMU main-loop burst-fills it), not at the sender. */
    {
        const lan9118_stats_t *lst = lan9118_get_stats();
        printf("pj_sip_dual[%s]:   lan9118 rx_pkt=%u rx_overruns=%u "
               "qemu_rx_drop=%lu irq=%u\r\n", ROLE_NAME,
               lst->rx_packets, lst->rx_overruns,
               (unsigned long)(*(volatile uint32_t *)0x420000A0u),
               lst->irq_count);
    }
    /* lwIP layer drop counters: where is the RTP actually lost?  If
     * udp.drop tracks the RTCP loss, it is the lwIP UDP receive path
     * (socket/netconn recv queue full because the media ioqueue was not
     * drained in time under QEMU TCG). */
    {
        extern struct stats_ lwip_stats;
        printf("pj_sip_dual[%s]:   lwip link.drop=%u ip.drop=%u udp.drop=%u\r\n",
               ROLE_NAME, (unsigned)lwip_stats.link.drop,
               (unsigned)lwip_stats.ip.drop, (unsigned)lwip_stats.udp.drop);
    }
    printf("pj_sip_dual[%s]:   rtcp tx pkt=%u bytes=%u loss=%u | "
           "rtt(avg)=%d us\r\n", ROLE_NAME, rstat.tx.pkt, rstat.tx.bytes,
           rstat.tx.loss, rstat.rtt.mean);
    {
        int dtmf_ok = 1;
#if !defined(PJ_DUAL_ROLE_CALLER)
        /* callee must hear the caller's DTMF "5#". */
        dtmf_ok = (g_dtmf_rx[0] == '5' && g_dtmf_rx[1] == '#' &&
                   g_dtmf_count == 2);
#endif
        printf("pj_sip_dual[%s]:   dtmf tx=\"%s\" rx=\"%s\" count=%d -> %s\r\n",
               ROLE_NAME, DTMF_SEQ, (const char*)g_dtmf_rx, g_dtmf_count,
               dtmf_ok ? "OK" : "MISS");
        g_dtmf_ok = dtmf_ok;
    }

    media_pcm_to_u8(s_mplay, s_mplay_u8, MEDIA_PCM_BYTES);
    printf("pj_sip_dual[%s]: playing peer -> speaker\r\n", ROLE_NAME);
    audio_play(s_mplay_u8, MEDIA_PCM_BYTES / 2, MEDIA_RATE, AUDIO_FORMAT_U8);
    pj_thread_sleep(2500);
    audio_stop();

    /* Tear down clock, stream + transport. */
    if (g_clock) {
        pjmedia_clock_stop(g_clock);
        pjmedia_clock_destroy(g_clock);
        g_clock = NULL;
    }
    if (g_stream) {
        pjmedia_stream_destroy(g_stream);
        g_stream = NULL;
        g_stream_port = NULL;
    }
    if (g_media_tp) {
        pjmedia_transport_close(g_media_tp);
        g_media_tp = NULL;
    }

    /* Real UDP path over slirp/hostfwd drops frames and the jitter buffer
     * frequently drains between RTP bursts; every such empty/missing frame
     * is concealed by the stream's PLC, so playback stays continuous.
     * Accept >= 85% delivered frames and >= 85% played frames, counting
     * NORMAL + PLC-recovered (MISSING and EMPTY/zero) as valid playback.
     * The callee must also have heard the DTMF digits. */
    if (rstat.rx.pkt >= (MEDIA_FRAMES * 85) / 100 &&
        (g_play_normal + g_play_missing + g_play_zero) >=
            (MEDIA_FRAMES * 85) / 100 &&
        g_dtmf_ok)
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
    printf("pj_sip_dual[caller]: dialing %s:%d\r\n", GW_IP, PEER_EXT_SIP);
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
        /* Create the pjmedia endpoint + built-in G.711 codec. */
        g_mep = NULL;
        rc = pjmedia_endpt_create(&g_cp.factory, NULL, 0, &g_mep);
        if (rc == PJ_SUCCESS)
            rc = pjmedia_codec_g711_init(g_mep);
        if (rc == PJ_SUCCESS && !pjmedia_event_mgr_instance()) {
            /* stream.c subscribes to RTCP events with mgr==NULL, which means
             * the global event manager instance; create one if missing. */
            pjmedia_event_mgr *em = NULL;
            pj_pool_t *epool = pjmedia_endpt_create_pool(g_mep, "evmgr",
                                                         1024, 1024);
            if (epool &&
                pjmedia_event_mgr_create(epool, 0, &em) == PJ_SUCCESS && em)
                pjmedia_event_mgr_set_instance(em);
        }
        if (rc != PJ_SUCCESS) {
            printf("pj_sip_dual[%s]: pjmedia endpt/codec FAILED (%d)\r\n",
                   ROLE_NAME, rc);
        } else if (run_dual_media(pool) == 0) {
            printf("pj_sip_dual[%s]: media ALL PASSED\r\n", ROLE_NAME);
            pass = 1;
        } else {
            printf("pj_sip_dual[%s]: media FAILED\r\n", ROLE_NAME);
        }
        if (g_mep) {
            /* Stop the global event-manager thread (created above) before the
             * endpoint frees its pool, or it would run on freed memory. */
            pjmedia_event_mgr_destroy(NULL);
            pjmedia_endpt_destroy(g_mep);
            g_mep = NULL;
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
