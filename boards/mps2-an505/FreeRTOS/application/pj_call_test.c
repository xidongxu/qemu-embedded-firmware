/*
 * pj_call_test.c - full call media test (stage 7): mic -> PCMU -> RTP ->
 * decode -> speaker, looped on one board (QEMU).
 *
 * This ties the real audio hardware (mpsx-mic capture, mpsx-audio playback)
 * into the PJMEDIA RTP/PCMU pipeline, which is what a real call does:
 *
 *   - mic_capture() records one segment of 8 kHz 16-bit PCM from the QEMU
 *     mic device (host side feeds it from a WAV file via
 *     -global mpsx-simple-mic.infile=audio_test_8k.wav).
 *   - Each 10 ms frame (80 samples) is u-law (PCMU) encoded and packed into
 *     an RTP packet, then sent over UDP to our own RTP port (loopback).
 *   - The same frames are received back, depacketized, PCMU-decoded and
 *     written into a playback buffer.
 *   - audio_play() plays the decoded PCM out of the mpsx-audio device, which
 *     QEMU captures to a WAV file (-audiodev wav,path=out.wav,id=a0
 *     -machine mps2-an505,audiodev=a0).
 *
 * If the WAV fed to the mic contains a 1 kHz tone, the WAV captured from the
 * speaker must contain a matching 1 kHz tone -> the whole call path works.
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

#define CALL_RTP_PORT   15066       /* local UDP port for the RTP loop */
#define CALL_RATE       8000        /* Hz */
#define CALL_FMT        MIC_FORMAT_S16   /* 16-bit linear PCM */
#define FRAME_SAMPLES   80          /* 10 ms @ 8 kHz */
#define FRAME_BYTES     (FRAME_SAMPLES * 2)   /* S16 */
#define CALL_FRAMES     200         /* 200 x 10 ms = 2 s of audio */
#define CALL_PCM_BYTES  (CALL_FRAMES * FRAME_BYTES)  /* 32000 bytes */

static pj_caching_pool   g_cp;
static pj_sock_t         g_rx_sock = PJ_INVALID_SOCKET;
static pj_sock_t         g_tx_sock = PJ_INVALID_SOCKET;

/* Static PCM buffers: mic capture in, decoded playback out. */
static uint8_t s_cap[CALL_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_play[CALL_PCM_BYTES] __attribute__((aligned(64)));
static uint8_t s_play_u8[CALL_PCM_BYTES / 2] __attribute__((aligned(64)));

int pj_call_test_run(void)
{
    pj_status_t rc;
    pj_pool_t *pool = NULL;
    pj_sockaddr_in local, dst;
    pj_sockaddr host;
    char ipstr[PJ_INET_ADDRSTRLEN];
    pjmedia_rtp_session tx_ses, rx_ses;
    pjmedia_rtp_status seq_st;
    pj_uint32_t ssrc = 0xCA111A5;
    int i;
    int rx_ok = 0, rx_bad = 0;

    printf("\r\n=== FULL CALL media test (mic->PCMU->RTP->decode->speaker) ===\r\n");

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

    /* Sockets: receiver bound to INADDR_ANY:RTP_PORT, separate TX socket. */
    rc = pj_gethostip(pj_AF_INET(), &host);
    if (rc != PJ_SUCCESS) {
        printf("pj_call: gethostip failed (%d)\r\n", rc);
        goto on_error;
    }
    pj_sockaddr_print(&host, ipstr, sizeof(ipstr), 0);
    printf("pj_call: local IP = %s\r\n", ipstr);

    rc = pj_sock_socket(PJ_AF_INET, PJ_SOCK_DGRAM, 0, &g_rx_sock);
    if (rc != PJ_SUCCESS) {
        printf("pj_call: rx socket failed (%d)\r\n", rc);
        goto on_error;
    }
    pj_sockaddr_in_init(&local, NULL, (pj_uint16_t)CALL_RTP_PORT);
    rc = pj_sock_bind(g_rx_sock, &local, sizeof(local));
    if (rc != PJ_SUCCESS) {
        printf("pj_call: bind failed (%d)\r\n", rc);
        goto on_error;
    }
    rc = pj_sock_socket(PJ_AF_INET, PJ_SOCK_DGRAM, 0, &g_tx_sock);
    if (rc != PJ_SUCCESS) {
        printf("pj_call: tx socket failed (%d)\r\n", rc);
        goto on_error;
    }
    pj_sockaddr_in_init(&dst, NULL, (pj_uint16_t)CALL_RTP_PORT);
    dst.sin_addr = host.ipv4.sin_addr;

    /* RTP sessions. */
    rc = pjmedia_rtp_session_init(&tx_ses, 0 /*PCMU*/, ssrc);
    if (rc != PJ_SUCCESS) { printf("pj_call: tx rtp init failed (%d)\r\n", rc); goto on_error; }
    rc = pjmedia_rtp_session_init(&rx_ses, 0 /*PCMU*/, 0);
    if (rc != PJ_SUCCESS) { printf("pj_call: rx rtp init failed (%d)\r\n", rc); goto on_error; }

    /* Record 2 s of 8 kHz 16-bit PCM from the mic device. */
    printf("pj_call: capturing %d bytes from mic @ %d Hz S16...\r\n",
           CALL_PCM_BYTES, CALL_RATE);
    if (!mic_capture(s_cap, CALL_PCM_BYTES, CALL_RATE, CALL_FMT, 20000000UL)) {
        printf("pj_call: mic_capture FAILED\r\n");
        goto on_error;
    }
    printf("pj_call: captured %d bytes\r\n", CALL_PCM_BYTES);

    /* Frame-by-frame: PCMU encode -> RTP -> UDP send; interleaved receive. */
    for (i = 0; i < CALL_FRAMES; ++i) {
        const void *rtphdr = NULL;
        int hdrlen = 0;
        pj_uint8_t ulaw[FRAME_SAMPLES];
        pj_uint8_t pkt[256];
        const uint8_t *frame = s_cap + (i * FRAME_BYTES);
        int k;

        /* PCM (S16) -> u-law (PCMU). */
        for (k = 0; k < FRAME_SAMPLES; ++k) {
            pj_int16_t s;
            memcpy(&s, frame + k * 2, 2);
            ulaw[k] = pjmedia_linear2ulaw(s);
        }

        /* RTP packetize. */
        rc = pjmedia_rtp_encode_rtp(&tx_ses, 0, 0, FRAME_SAMPLES,
                                    FRAME_SAMPLES, &rtphdr, &hdrlen);
        if (rc != PJ_SUCCESS) {
            printf("pj_call: encode failed (%d)\r\n", rc);
            break;
        }
        memcpy(pkt, rtphdr, (size_t)hdrlen);
        memcpy(pkt + hdrlen, ulaw, sizeof(ulaw));

        {
            pj_ssize_t len = hdrlen + (int)sizeof(ulaw);
            rc = pj_sock_sendto(g_tx_sock, pkt, &len, 0, &dst, sizeof(dst));
            if (rc != PJ_SUCCESS) {
                printf("pj_call: sendto failed (%d)\r\n", rc);
                break;
            }
        }

        /* Interleaved receive: give lwIP time to deliver then read one frame. */
        {
            pj_uint8_t buf[256];
            pj_ssize_t rlen = (pj_ssize_t)sizeof(buf);
            const pjmedia_rtp_hdr *hdr = NULL;
            const void *payload = NULL;
            unsigned plen = 0;
            const pj_uint8_t *ul;
            uint8_t *out = s_play + (i * FRAME_BYTES);
            pj_fd_set_t rfds;
            pj_time_val stmo;

            pj_thread_sleep(3);

            PJ_FD_ZERO(&rfds);
            PJ_FD_SET(g_rx_sock, &rfds);
            stmo.sec = 2;
            stmo.msec = 0;
            if (pj_sock_select((int)g_rx_sock + 1, &rfds, NULL, NULL,
                               &stmo) <= 0 ||
                !PJ_FD_ISSET(g_rx_sock, &rfds))
            {
                printf("pj_call: recv timeout at frame %d\r\n", i);
                rx_bad++;
                break;
            }

            rc = pj_sock_recvfrom(g_rx_sock, buf, &rlen, 0, NULL, NULL);
            if (rc != PJ_SUCCESS) {
                printf("pj_call: recvfrom failed (%d)\r\n", rc);
                break;
            }

            rc = pjmedia_rtp_decode_rtp(&rx_ses, buf, (int)rlen, &hdr,
                                        &payload, &plen);
            if (rc != PJ_SUCCESS) {
                printf("pj_call: decode failed (%d)\r\n", rc);
                rx_bad++;
                continue;
            }
            pjmedia_rtp_session_update(&rx_ses, hdr, &seq_st);
            if (seq_st.status.flag.bad) {
                printf("pj_call: bad RTP packet\r\n");
                rx_bad++;
                continue;
            }

            /* PCMU -> PCM (S16), write into the playback buffer. */
            ul = (const pj_uint8_t*)payload;
            if (plen > FRAME_SAMPLES)
                plen = FRAME_SAMPLES;
            for (k = 0; k < (int)plen; ++k) {
                pj_int16_t s = (pj_int16_t)pjmedia_ulaw2linear(ul[k]);
                out[k * 2]     = (uint8_t)(s & 0xFF);
                out[k * 2 + 1] = (uint8_t)((s >> 8) & 0xFF);
            }
            rx_ok++;
        }
    }

    /* Check captured source has energy (signal present). */
    {
        pj_int32_t peak = 0;
        int seg;
        printf("pj_call: mic source per-250ms RMS:\r\n");
        for (seg = 0; seg < 8; ++seg) {
            pj_uint64_t acc = 0;
            int cnt = (CALL_PCM_BYTES / 2) / 8;
            for (i = seg * cnt; i < (seg + 1) * cnt; ++i) {
                pj_int16_t s;
                memcpy(&s, s_cap + i * 2, 2);
                acc += (pj_uint64_t)(s < 0 ? -s : s);
                if (s < 0) s = (pj_int16_t)(-s);
                if (s > peak) peak = s;
            }
            printf("  [%d] mean_abs=%lu\r\n", seg,
                   (unsigned long)(acc / cnt));
        }
        printf("pj_call: mic source peak=%ld\r\n", (long)peak);
        if (peak < 200) {
            printf("pj_call: mic source too weak (check WAV infile / rate)\r\n");
            rx_bad = CALL_FRAMES + 1;
        }
    }

    printf("pj_call: RTP frames tx=40 rx ok=%d bad=%d\r\n", rx_ok, rx_bad);

    /* Diagnostics on the decoded playback buffer. */
    {
        pj_int32_t peak = 0;
        int seg;
        printf("pj_call: playback buffer per-250ms RMS:\r\n");
        for (seg = 0; seg < 8; ++seg) {
            pj_uint64_t acc = 0;
            int cnt = (CALL_PCM_BYTES / 2) / 8;
            for (i = seg * cnt; i < (seg + 1) * cnt; ++i) {
                pj_int16_t s;
                memcpy(&s, s_play + i * 2, 2);
                acc += (pj_uint64_t)(s < 0 ? -s : s);
                if (s < 0) s = (pj_int16_t)(-s);
                if (s > peak) peak = s;
            }
            printf("  [%d] mean_abs=%lu\r\n", seg,
                   (unsigned long)(acc / cnt));
        }
        printf("pj_call: playback peak=%ld\r\n", (long)peak);
    }

    /* Convert decoded S16 PCM to U8 for playback.  The QEMU mpsx-simple-audio
     * device keeps the voice format it was first opened with (audio_test
     * opens it as U8), and re-opening on a later S16 format write is skipped
     * because the device is disabled at that point -> S16 data would be
     * played back as U8 (freq halved).  Playing U8 is the verified path. */
    for (i = 0; i < CALL_PCM_BYTES / 2; ++i) {
        pj_int16_t s;
        memcpy(&s, s_play + i * 2, 2);
        s_play_u8[i] = (uint8_t)(((int32_t)s >> 8) + 128);
    }

    /* Play the decoded PCM out of the speaker (QEMU captures to WAV). */
    printf("pj_call: playing %d bytes decoded PCM @ %d Hz U8...\r\n",
           CALL_PCM_BYTES / 2, CALL_RATE);
    audio_play(s_play_u8, CALL_PCM_BYTES / 2, CALL_RATE, AUDIO_FORMAT_U8);

    if (rx_ok == CALL_FRAMES && rx_bad == 0) {
        printf("pj_call: ALL PASSED\r\n");
        rc = 0;
    } else {
        printf("pj_call: FAILED (rx_ok=%d rx_bad=%d)\r\n", rx_ok, rx_bad);
        rc = -1;
    }

    /* Let the speaker loop for a moment so the WAV backend captures audio. */
    pj_thread_sleep(3000);
    audio_stop();

    if (g_rx_sock != PJ_INVALID_SOCKET) pj_sock_close(g_rx_sock);
    if (g_tx_sock != PJ_INVALID_SOCKET) pj_sock_close(g_tx_sock);
    pj_pool_release(pool);
    pj_caching_pool_destroy(&g_cp);
    pj_shutdown();
    printf("pj_call: DONE\r\n");
    return rc;

on_error:
    if (g_rx_sock != PJ_INVALID_SOCKET) pj_sock_close(g_rx_sock);
    if (g_tx_sock != PJ_INVALID_SOCKET) pj_sock_close(g_tx_sock);
    if (pool)
        pj_pool_release(pool);
    pj_caching_pool_destroy(&g_cp);
    pj_shutdown();
    printf("pj_call: FAILED\r\n");
    return -1;
}
