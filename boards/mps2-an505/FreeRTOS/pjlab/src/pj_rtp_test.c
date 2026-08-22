/*
 * pj_rtp_test.c - PJSIP/PJMEDIA RTP media loopback self-test (stage 6).
 *
 * Verifies the media plane that "making a call" needs, on top of the
 * CONFIRMED INVITE session:
 *   - G.711 PCMU (u-law) encode/decode (pjmedia alaw_ulaw tables)
 *   - RTP packetization / depacketization (pjmedia_rtp_session)
 *   - RTP-over-UDP send/recv over the lwIP socket layer
 *
 * Design (loopback, no external peer needed):
 *   - A synthetic 1 kHz / 8 kHz sine wave is PCMU-encoded, packed into RTP
 *     packets (pt=0, ssrc fixed), and sent over UDP to our own socket.
 *   - The same task reads the RTP packets back, depacketizes, PCMU-decodes
 *     and checks the waveform (energy present, ~1 kHz dominant).
 *
 * The test is independent of the INVITE test but uses the same socket layer,
 * so it proves the RTP/PCMU pipeline that a real call would use.
 */

#include <stdio.h>
#include <string.h>
#include "printf.h"
#include "FreeRTOS.h"
#include "task.h"
#include "pj_rtp_test.h"

#include <pj/pool.h>
#include <pj/os.h>
#include <pj/log.h>
#include <pj/string.h>
#include <pj/sock.h>
#include <pj/sock_select.h>
#include <pj/addr_resolv.h>
#include <pjlib-util/string.h>
#include <pjmedia/rtp.h>
#include <pjmedia/alaw_ulaw.h>

#define RTP_TEST_PORT   15064       /* local UDP port for the RTP loop */
#define RTP_PT_PCMU     0
#define RTP_SAMPLES     80          /* 10 ms @ 8 kHz = 80 samples */
#define RTP_FRAMES      40          /* 40 x 10 ms = 400 ms of audio */
#define RTP_SRATE       8000
#define RTP_TONE_FREQ   1000        /* Hz */

/* Fixed-point sine LUT for the test tone (no libm dependency). */
#define SINE_BITS       6
#define SINE_LEN        (1u << SINE_BITS)
static const pj_int16_t sine_lut[SINE_LEN] = {
     0,  1592, 3140, 4587, 5880, 6979, 7857, 8392, 8579, 8392, 7857, 6979,
  5880, 4587, 3140, 1592,    0, -1592,-3140,-4587,-5880,-6979,-7857,-8392,
 -8579,-8392,-7857,-6979,-5880,-4587,-3140,-1592,    0, 1592, 3140, 4587,
  5880, 6979, 7857, 8392, 8579, 8392, 7857, 6979, 5880, 4587, 3140, 1592,
     0, -1592,-3140,-4587,-5880,-6979,-7857,-8392,-8579,-8392,-7857,-6979,
 -5880,-4587,-3140,-1592
};
/* DDS phase increment for 1 kHz @ 8 kHz with SINE_BITS fraction. */
#define sine_step       ((pj_uint32_t)(((pj_uint64_t)RTP_TONE_FREQ << (32 - SINE_BITS)) / RTP_SRATE))

#define SET_STR(s, lit) do { (s).ptr = (char*)(lit); \
                             (s).slen = (pj_ssize_t)(sizeof(lit)-1); } while (0)

static pj_caching_pool   g_cp;
static pj_sock_t         g_rx_sock = PJ_INVALID_SOCKET;
static pj_sock_t         g_tx_sock = PJ_INVALID_SOCKET;

/* ------------------------------------------------------------------ */
/* Main entry                                                          */
/* ------------------------------------------------------------------ */
int pj_rtp_test_run(void)
{
    pj_status_t rc;
    pj_pool_t *pool = NULL;
    pj_sockaddr_in local, dst;
    pj_sockaddr host;
    char ipstr[PJ_INET_ADDRSTRLEN];
    pjmedia_rtp_session tx_ses, rx_ses;
    pjmedia_rtp_status seq_st;
    pj_uint32_t ssrc = 0x12345678;
    int i;

    printf("\r\n=== PJMEDIA RTP/PCMU loopback test ===\r\n");

    rc = pj_init();
    if (rc != PJ_SUCCESS) {
        printf("pj_rtp: pj_init failed (%d)\r\n", rc);
        return -1;
    }
    pj_caching_pool_init(&g_cp, &pj_pool_factory_default_policy, 0);
    pool = pj_pool_create(&g_cp.factory, "rtp", 2048, 1024, NULL);
    if (!pool) {
        printf("pj_rtp: pool failed\r\n");
        goto on_error;
    }

    /* Two UDP sockets: a receiver bound to INADDR_ANY:RTP_TEST_PORT and a
     * separate transmitter.  This mirrors pj_net_test (which works on the
     * lwIP loopback). */
    rc = pj_gethostip(pj_AF_INET(), &host);
    if (rc != PJ_SUCCESS) {
        printf("pj_rtp: gethostip failed (%d)\r\n", rc);
        goto on_error;
    }
    pj_sockaddr_print(&host, ipstr, sizeof(ipstr), 0);
    printf("pj_rtp: local IP = %s\r\n", ipstr);

    /* Receiver: bind INADDR_ANY:RTP_TEST_PORT. */
    rc = pj_sock_socket(PJ_AF_INET, PJ_SOCK_DGRAM, 0, &g_rx_sock);
    if (rc != PJ_SUCCESS) {
        printf("pj_rtp: rx socket failed (%d)\r\n", rc);
        goto on_error;
    }
    pj_sockaddr_in_init(&local, NULL, (pj_uint16_t)RTP_TEST_PORT);  /* INADDR_ANY */
    rc = pj_sock_bind(g_rx_sock, &local, sizeof(local));
    if (rc != PJ_SUCCESS) {
        printf("pj_rtp: bind failed (%d)\r\n", rc);
        goto on_error;
    }

    /* Transmitter socket (unbound; kernel picks source addr). */
    rc = pj_sock_socket(PJ_AF_INET, PJ_SOCK_DGRAM, 0, &g_tx_sock);
    if (rc != PJ_SUCCESS) {
        printf("pj_rtp: tx socket failed (%d)\r\n", rc);
        goto on_error;
    }

    /* Destination = local IP:RTP_TEST_PORT (loopback). */
    pj_sockaddr_in_init(&dst, NULL, (pj_uint16_t)RTP_TEST_PORT);
    dst.sin_addr = host.ipv4.sin_addr;

    /* RTP sessions: TX and RX. */
    rc = pjmedia_rtp_session_init(&tx_ses, RTP_PT_PCMU, ssrc);
    if (rc != PJ_SUCCESS) {
        printf("pj_rtp: tx rtp init failed (%d)\r\n", rc);
        goto on_error;
    }
    rc = pjmedia_rtp_session_init(&rx_ses, RTP_PT_PCMU, 0);
    if (rc != PJ_SUCCESS) {
        printf("pj_rtp: rx rtp init failed (%d)\r\n", rc);
        goto on_error;
    }

    printf("pj_rtp: sending %d PCMU RTP frames (%.1f kHz tone @ %d Hz)\r\n",
           RTP_FRAMES, (double)RTP_SRATE/1000.0, RTP_TONE_FREQ);

    {
        pj_uint8_t pcm[RTP_SAMPLES * 2];   /* 16-bit linear PCM */
        pj_uint8_t ulaw[RTP_SAMPLES];      /* u-law encoded      */
        pj_uint8_t pkt[512];               /* RTP packet buffer  */
        int rx_ok = 0, rx_bad = 0;
        pj_uint32_t rx_pkts = 0;
        pj_uint32_t phase_acc = 0;         /* fixed-point phase accumulator */

        for (i = 0; i < RTP_FRAMES; ++i) {
            const void *rtphdr = NULL;
            int hdrlen = 0;
            pj_uint16_t *p;
            int k;

            /* Generate one frame of 1 kHz sine wave @ 8 kHz, 16-bit PCM.
             * Fixed-point DDS: step = freq<<SINE_BITS / rate. */
            p = (pj_uint16_t*)pcm;
            for (k = 0; k < RTP_SAMPLES; ++k) {
                pj_uint32_t idx = phase_acc >> (32 - SINE_BITS);
                pj_int32_t s = sine_lut[idx];
                *p++ = (pj_uint16_t)s;
                phase_acc += sine_step;
            }

            /* Encode to u-law (PCMU). */
            for (k = 0; k < RTP_SAMPLES; ++k) {
                pj_int16_t s;
                memcpy(&s, &pcm[k*2], 2);
                ulaw[k] = pjmedia_linear2ulaw(s);
            }

            /* Build RTP packet: header + payload. */
            rc = pjmedia_rtp_encode_rtp(&tx_ses, RTP_PT_PCMU, 0,
                                        RTP_SAMPLES, RTP_SAMPLES,
                                        &rtphdr, &hdrlen);
            if (rc != PJ_SUCCESS) {
                printf("pj_rtp: encode failed (%d)\r\n", rc);
                break;
            }
            memcpy(pkt, rtphdr, (size_t)hdrlen);
            memcpy(pkt + hdrlen, ulaw, sizeof(ulaw));

            {
                pj_ssize_t len = hdrlen + (int)sizeof(ulaw);
                rc = pj_sock_sendto(g_tx_sock, pkt, &len, 0, &dst, sizeof(dst));
                if (rc != PJ_SUCCESS) {
                    printf("pj_rtp: sendto failed (%d)\r\n", rc);
                    break;
                }
            }

            /* Interleaved receive: give lwIP time to deliver then read one
             * frame back (mimics full-duplex RTP pacing at 10 ms/frame). */
            {
                pj_uint8_t buf[512];
                pj_ssize_t rlen = (pj_ssize_t)sizeof(buf);
                const pjmedia_rtp_hdr *hdr = NULL;
                const void *payload = NULL;
                unsigned plen = 0;
                pj_int32_t peak = 0;
                const pj_uint8_t *ul;
                pj_fd_set_t rfds;
                pj_time_val stmo;
                int j;

                pj_thread_sleep(3);

                PJ_FD_ZERO(&rfds);
                PJ_FD_SET(g_rx_sock, &rfds);
                stmo.sec = 2;
                stmo.msec = 0;
                if (pj_sock_select((int)g_rx_sock + 1, &rfds, NULL, NULL,
                                   &stmo) <= 0 ||
                    !PJ_FD_ISSET(g_rx_sock, &rfds))
                {
                    printf("pj_rtp: recv timeout at frame %d\r\n", i);
                    rx_bad++;
                    break;
                }

                rc = pj_sock_recvfrom(g_rx_sock, buf, &rlen, 0, NULL, NULL);
                if (rc != PJ_SUCCESS) {
                    printf("pj_rtp: recvfrom failed (%d)\r\n", rc);
                    break;
                }

                rc = pjmedia_rtp_decode_rtp(&rx_ses, buf, (int)rlen, &hdr,
                                            &payload, &plen);
                if (rc != PJ_SUCCESS) {
                    printf("pj_rtp: decode failed (%d)\r\n", rc);
                    rx_bad++;
                    continue;
                }
                pjmedia_rtp_session_update(&rx_ses, hdr, &seq_st);
                if (seq_st.status.flag.bad) {
                    printf("pj_rtp: bad RTP packet\r\n");
                    rx_bad++;
                    continue;
                }

                /* Decode u-law back to linear PCM and compute peak. */
                ul = (const pj_uint8_t*)payload;
                for (j = 0; j < (int)plen && j < RTP_SAMPLES; ++j) {
                    pj_int16_t s = (pj_int16_t)pjmedia_ulaw2linear(ul[j]);
                    if (s < 0) s = (pj_int16_t)(-s);
                    if (s > peak) peak = s;
                }
                if (plen != RTP_SAMPLES || peak < 2000) {
                    printf("pj_rtp: frame %d: plen=%u peak=%ld (unexpected)\r\n",
                           i, plen, (long)peak);
                    rx_bad++;
                } else {
                    rx_ok++;
                }
                rx_pkts++;
            }
        }

        printf("pj_rtp: received %lu RTP packets, ok=%d bad=%d\r\n",
               (unsigned long)rx_pkts, rx_ok, rx_bad);
        printf("pj_rtp: %s\r\n",
               (rx_pkts == (pj_uint32_t)RTP_FRAMES && rx_ok == RTP_FRAMES &&
                rx_bad == 0) ? "ALL PASSED" : "FAILED");
        if (!(rx_pkts == (pj_uint32_t)RTP_FRAMES && rx_ok == RTP_FRAMES &&
              rx_bad == 0)) {
            goto on_error;
        }
    }

    pj_sock_close(g_rx_sock);
    pj_sock_close(g_tx_sock);
    pj_pool_release(pool);
    pj_caching_pool_destroy(&g_cp);
    pj_shutdown();
    printf("pj_rtp: DONE\r\n");
    return 0;

on_error:
    if (g_rx_sock != PJ_INVALID_SOCKET)
        pj_sock_close(g_rx_sock);
    if (g_tx_sock != PJ_INVALID_SOCKET)
        pj_sock_close(g_tx_sock);
    if (pool)
        pj_pool_release(pool);
    pj_caching_pool_destroy(&g_cp);
    pj_shutdown();
    printf("pj_rtp: FAILED\r\n");
    return -1;
}
