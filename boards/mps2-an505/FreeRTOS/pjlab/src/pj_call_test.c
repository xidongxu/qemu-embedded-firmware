/*
 * pj_call_test.c - full-duplex call media test (stage 8): two endpoints
 * (A and B) each run their own RTP/PCMU media channel simultaneously, which
 * is what a real two-way call does.
 *
 *   Endpoint A (caller):  RX on 10.0.2.15:15066, TX to 10.0.2.15:15068
 *   Endpoint B (callee):  RX on 10.0.2.15:15068, TX to 10.0.2.15:15066
 *
 * Media path (per direction, identical to a real call):
 *   mic (8 kHz S16, WAV-fed) -> PCMU encode -> RTP -> UDP -> RTP decode ->
 *   PCMU decode -> U8 -> speaker (QEMU wav capture).
 *
 * Direction A->B uses capture buffer capA -> playback buf playB;
 * direction B->A uses capture buffer capB -> playback buf playA.
 * Both directions are verified independently (signal present + peak).
 *
 * QEMU:
 *   -global mpsx-simple-mic.infile=<1k.wav>
 *   -audiodev wav,path=out.wav,id=a0 -machine mps2-an505,audiodev=a0
 */

#include <stdio.h>
#include <string.h>
#include "printf.h"
#include "FreeRTOS.h"
#include "task.h"
#include "pj_call_test.h"

#include <pj/pool.h>
#include <pj/os.h>
#include <pj/log.h>
#include <pj/string.h>
#include <pj/sock.h>
#include <pj/sock_select.h>
#include <pj/addr_resolv.h>
#include <pjmedia/rtp.h>
#include <pjmedia/alaw_ulaw.h>

#include "mic.h"
#include "audio.h"

#define CALL_RATE       8000        /* Hz */
#define CALL_FMT        MIC_FORMAT_S16   /* 16-bit linear PCM */
#define FRAME_SAMPLES   80          /* 10 ms @ 8 kHz */
#define FRAME_BYTES     (FRAME_SAMPLES * 2)   /* S16 */
#define CALL_FRAMES     200         /* 200 x 10 ms = 2 s of audio */
#define CALL_PCM_BYTES  (CALL_FRAMES * FRAME_BYTES)  /* 32000 bytes */

#define PORT_A          15066       /* endpoint A RTP port (B sends here) */
#define PORT_B          15068       /* endpoint B RTP port (A sends here) */

static pj_caching_pool   g_cp;

/* One RTP media endpoint. */
typedef struct call_ep {
    pj_sock_t         rx;           /* receiver socket (bound)   */
    pj_sock_t         tx;           /* transmitter socket        */
    pj_sockaddr_in    peer;         /* peer destination address  */
    pjmedia_rtp_session rtp;        /* RTP session state         */
    const uint8_t    *cap;          /* captured PCM (source)     */
    uint8_t          *play;         /* decoded PCM (playback)    */
    pj_uint32_t       ssrc;
} call_ep;

/* Static PCM buffers: two capture sources + two playback buffers. */
static uint8_t s_capA[CALL_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_capB[CALL_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_playA[CALL_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_playB[CALL_PCM_BYTES] __attribute__((aligned(64)));
/* U8 versions for the speaker (QEMU wav backend). */
static uint8_t s_playA_u8[CALL_PCM_BYTES / 2] __attribute__((aligned(64)));
static uint8_t s_playB_u8[CALL_PCM_BYTES / 2] __attribute__((aligned(64)));

/* Open the sockets for one endpoint. */
static pj_status_t ep_open(call_ep *ep, const pj_sockaddr_in *peer_ip,
                           pj_uint16_t rx_port, pj_uint16_t tx_port,
                           pj_uint32_t ssrc)
{
    pj_sockaddr_in local;
    pj_status_t rc;

    rc = pj_sock_socket(PJ_AF_INET, PJ_SOCK_DGRAM, 0, &ep->rx);
    if (rc != PJ_SUCCESS) return rc;
    pj_sockaddr_in_init(&local, NULL, rx_port);   /* INADDR_ANY */
    rc = pj_sock_bind(ep->rx, &local, sizeof(local));
    if (rc != PJ_SUCCESS) return rc;

    rc = pj_sock_socket(PJ_AF_INET, PJ_SOCK_DGRAM, 0, &ep->tx);
    if (rc != PJ_SUCCESS) return rc;

    /* Destination is the peer's RX port on our own IP (loopback). */
    pj_sockaddr_in_init(&ep->peer, NULL, tx_port);
    ep->peer.sin_addr = peer_ip->sin_addr;

    ep->ssrc = ssrc;
    return pjmedia_rtp_session_init(&ep->rtp, 0 /*PCMU*/, ssrc);
}

/* Transmit one frame: PCM(S16) -> PCMU -> RTP -> UDP. */
static pj_status_t ep_send(call_ep *ep, int frame)
{
    const uint8_t *frame_pcm = ep->cap + frame * FRAME_BYTES;
    pj_uint8_t ulaw[FRAME_SAMPLES];
    pj_uint8_t pkt[256];
    const void *rtphdr = NULL;
    int hdrlen = 0;
    int k;
    pj_status_t rc;

    for (k = 0; k < FRAME_SAMPLES; ++k) {
        pj_int16_t s;
        memcpy(&s, frame_pcm + k * 2, 2);
        ulaw[k] = pjmedia_linear2ulaw(s);
    }
    rc = pjmedia_rtp_encode_rtp(&ep->rtp, 0, 0, FRAME_SAMPLES,
                                FRAME_SAMPLES, &rtphdr, &hdrlen);
    if (rc != PJ_SUCCESS)
        return rc;
    memcpy(pkt, rtphdr, (size_t)hdrlen);
    memcpy(pkt + hdrlen, ulaw, sizeof(ulaw));
    {
        pj_ssize_t len = hdrlen + (int)sizeof(ulaw);
        return pj_sock_sendto(ep->tx, pkt, &len, 0, &ep->peer, sizeof(ep->peer));
    }
}

/* Receive one frame: UDP -> RTP decode -> PCMU decode -> PCM playback. */
static int ep_recv(call_ep *ep, int frame)
{
    pj_uint8_t buf[256];
    pj_ssize_t len = (pj_ssize_t)sizeof(buf);
    const pjmedia_rtp_hdr *hdr = NULL;
    const void *payload = NULL;
    unsigned plen = 0;
    pjmedia_rtp_status seq_st;
    const pj_uint8_t *ul;
    uint8_t *out = ep->play + frame * FRAME_BYTES;
    pj_fd_set_t rfds;
    pj_time_val stmo;
    int k;
    pj_status_t rc;

    PJ_FD_ZERO(&rfds);
    PJ_FD_SET(ep->rx, &rfds);
    stmo.sec = 2;
    stmo.msec = 0;
    if (pj_sock_select((int)ep->rx + 1, &rfds, NULL, NULL, &stmo) <= 0 ||
        !PJ_FD_ISSET(ep->rx, &rfds))
        return -1;

    rc = pj_sock_recvfrom(ep->rx, buf, &len, 0, NULL, NULL);
    if (rc != PJ_SUCCESS)
        return -1;

    rc = pjmedia_rtp_decode_rtp(&ep->rtp, buf, (int)len, &hdr, &payload, &plen);
    if (rc != PJ_SUCCESS)
        return -1;
    pjmedia_rtp_session_update(&ep->rtp, hdr, &seq_st);
    if (seq_st.status.flag.bad)
        return -1;

    ul = (const pj_uint8_t*)payload;
    if (plen > FRAME_SAMPLES)
        plen = FRAME_SAMPLES;
    for (k = 0; k < (int)plen; ++k) {
        pj_int16_t s = (pj_int16_t)pjmedia_ulaw2linear(ul[k]);
        out[k * 2]     = (uint8_t)(s & 0xFF);
        out[k * 2 + 1] = (uint8_t)((s >> 8) & 0xFF);
    }
    return 0;
}

/* Convert S16 PCM buffer to U8 (silence=0x80) for the audio device. */
static void pcm_s16_to_u8(const uint8_t *s16, uint8_t *u8, int bytes)
{
    int i;
    for (i = 0; i < bytes / 2; ++i) {
        pj_int16_t s;
        memcpy(&s, s16 + i * 2, 2);
        u8[i] = (uint8_t)(128 + (s >> 8));
    }
}

/* Report signal stats for one buffer. */
static void report_buf(const char *name, const uint8_t *pcm, int bytes)
{
    pj_int32_t peak = 0;
    int i;
    for (i = 0; i < bytes / 2; ++i) {
        pj_int16_t s;
        memcpy(&s, pcm + i * 2, 2);
        if (s < 0) s = (pj_int16_t)(-s);
        if (s > peak) peak = s;
    }
    printf("pj_call: %s peak=%ld\r\n", name, (long)peak);
}

int pj_call_test_run(void)
{
    pj_status_t rc;
    pj_pool_t *pool = NULL;
    pj_sockaddr host;
    char ipstr[PJ_INET_ADDRSTRLEN];
    call_ep epA, epB;
    int i;
    int okA = 0, okB = 0, badA = 0, badB = 0;

    memset(&epA, 0, sizeof(epA));
    memset(&epB, 0, sizeof(epB));
    epA.rx = epA.tx = PJ_INVALID_SOCKET;
    epB.rx = epB.tx = PJ_INVALID_SOCKET;

    printf("\r\n=== FULL-DUPLEX CALL media test (A<->B RTP/PCMU) ===\r\n");

    rc = pj_init();
    if (rc != PJ_SUCCESS) {
        printf("pj_call: pj_init failed (%d)\r\n", rc);
        return -1;
    }
    pj_caching_pool_init(&g_cp, &pj_pool_factory_default_policy, 0);
    pool = pj_pool_create(&g_cp.factory, "call", 2048, 1024, NULL);
    if (!pool) {
        printf("pj_call: pool failed\r\n");
        goto on_error;
    }

    rc = pj_gethostip(pj_AF_INET(), &host);
    if (rc != PJ_SUCCESS) {
        printf("pj_call: gethostip failed (%d)\r\n", rc);
        goto on_error;
    }
    pj_sockaddr_print(&host, ipstr, sizeof(ipstr), 0);
    printf("pj_call: local IP = %s\r\n", ipstr);

    /* Open both endpoints. A sends to port B, B sends to port A. */
    rc = ep_open(&epA, &host.ipv4, PORT_A, PORT_B, 0xAAA00001);
    if (rc != PJ_SUCCESS) { printf("pj_call: epA open failed (%d)\r\n", rc); goto on_error; }
    rc = ep_open(&epB, &host.ipv4, PORT_B, PORT_A, 0xBBB00001);
    if (rc != PJ_SUCCESS) { printf("pj_call: epB open failed (%d)\r\n", rc); goto on_error; }
    epA.cap = s_capA; epA.play = s_playA;
    epB.cap = s_capB; epB.play = s_playB;

    /* Record two independent 2 s segments from the mic device. */
    printf("pj_call: capturing source A (%d bytes @ %d Hz S16)...\r\n",
           CALL_PCM_BYTES, CALL_RATE);
    if (!mic_capture(s_capA, CALL_PCM_BYTES, CALL_RATE, CALL_FMT, 20000000UL)) {
        printf("pj_call: mic_capture A FAILED\r\n");
        goto on_error;
    }
    printf("pj_call: capturing source B (%d bytes @ %d Hz S16)...\r\n",
           CALL_PCM_BYTES, CALL_RATE);
    if (!mic_capture(s_capB, CALL_PCM_BYTES, CALL_RATE, CALL_FMT, 20000000UL)) {
        printf("pj_call: mic_capture B FAILED\r\n");
        goto on_error;
    }
    report_buf("source A peak", s_capA, CALL_PCM_BYTES);
    report_buf("source B peak", s_capB, CALL_PCM_BYTES);

    /* Run both directions interleaved (each 10 ms frame). */
    for (i = 0; i < CALL_FRAMES; ++i) {
        /* A sends frame i to B, B sends frame i to A. */
        if (ep_send(&epA, i) == PJ_SUCCESS &&
            ep_send(&epB, i) == PJ_SUCCESS)
        {
            pj_thread_sleep(3);   /* let lwIP deliver before recv */
            if (ep_recv(&epA, i) == 0) okA++; else badA++;
            if (ep_recv(&epB, i) == 0) okB++; else badB++;
        } else {
            printf("pj_call: send failed at frame %d\r\n", i);
            break;
        }
    }

    printf("pj_call: dir B->A (A received): ok=%d bad=%d\r\n", okA, badA);
    printf("pj_call: dir A->B (B received): ok=%d bad=%d\r\n", okB, badB);
    report_buf("playback A peak", s_playA, CALL_PCM_BYTES);
    report_buf("playback B peak", s_playB, CALL_PCM_BYTES);

    /* Convert both playback buffers to U8 and play them (A then B). */
    pcm_s16_to_u8(s_playA, s_playA_u8, CALL_PCM_BYTES);
    pcm_s16_to_u8(s_playB, s_playB_u8, CALL_PCM_BYTES);
    printf("pj_call: playing A->speaker (%d bytes U8)...\r\n", CALL_PCM_BYTES / 2);
    audio_play(s_playA_u8, CALL_PCM_BYTES / 2, CALL_RATE, AUDIO_FORMAT_U8);
    pj_thread_sleep(2500);
    printf("pj_call: playing B->speaker (%d bytes U8)...\r\n", CALL_PCM_BYTES / 2);
    audio_play(s_playB_u8, CALL_PCM_BYTES / 2, CALL_RATE, AUDIO_FORMAT_U8);
    pj_thread_sleep(2500);
    audio_stop();

    if (okA == CALL_FRAMES && okB == CALL_FRAMES &&
        badA == 0 && badB == 0) {
        printf("pj_call: ALL PASSED (both directions full-duplex)\r\n");
        rc = 0;
    } else {
        printf("pj_call: FAILED\r\n");
        rc = -1;
    }

    if (epA.rx != PJ_INVALID_SOCKET) pj_sock_close(epA.rx);
    if (epA.tx != PJ_INVALID_SOCKET) pj_sock_close(epA.tx);
    if (epB.rx != PJ_INVALID_SOCKET) pj_sock_close(epB.rx);
    if (epB.tx != PJ_INVALID_SOCKET) pj_sock_close(epB.tx);
    pj_pool_release(pool);
    pj_caching_pool_destroy(&g_cp);
    pj_shutdown();
    printf("pj_call: DONE\r\n");
    return rc;

on_error:
    if (epA.rx != PJ_INVALID_SOCKET) pj_sock_close(epA.rx);
    if (epA.tx != PJ_INVALID_SOCKET) pj_sock_close(epA.tx);
    if (epB.rx != PJ_INVALID_SOCKET) pj_sock_close(epB.rx);
    if (epB.tx != PJ_INVALID_SOCKET) pj_sock_close(epB.tx);
    if (pool)
        pj_pool_release(pool);
    pj_caching_pool_destroy(&g_cp);
    pj_shutdown();
    printf("pj_call: FAILED\r\n");
    return -1;
}
