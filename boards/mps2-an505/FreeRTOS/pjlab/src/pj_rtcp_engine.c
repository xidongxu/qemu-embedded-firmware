/*
 * pj_rtcp_engine.c - minimal RTCP (RFC 3550 subset) for the call test.
 *
 * See pj_rtcp_engine.h for design notes.  The "NTP" fields of SR carry the
 * low 32 bits of the system tick (pj_get_timestamp) and LSR/DLSR are in the
 * same tick units, so RTT = A - LSR - DLSR is self-consistent.
 */

#include "pj_rtcp_engine.h"
#include <pj/sock.h>
#include <pj/assert.h>
#include <pj/log.h>
#include <string.h>

#define THIS_FILE "pj_rtcp_engine"

void rtcp_init(rtcp_session *s, pj_uint32_t ssrc, unsigned clock_rate,
               pj_uint32_t peer_ssrc, const char *cname)
{
    memset(s, 0, sizeof(*s));
    s->ssrc = ssrc;
    s->clock_rate = clock_rate;
    s->peer_ssrc = peer_ssrc;
    s->cname = cname;
}

void rtcp_tx_rtp(rtcp_session *s, unsigned payload_len)
{
    s->tx.pkt_count++;
    s->tx.octet_count += payload_len;
}

void rtcp_rx_rtp(rtcp_session *s, unsigned seq, unsigned rtp_ts)
{
    pj_timestamp now, freq;
    pj_int64_t arrival;
    pj_int32_t transit;

    if (s->rx.received == 0) {
        s->rx.base_seq = seq;
    } else if ((pj_int32_t)(seq - s->rx.max_seq) > 0) {
        s->rx.max_seq = seq;
    }
    s->rx.received++;

    /* Interarrival jitter (RFC 3550 A.8): transit = arrival - rtp_ts,
     * in RTP clock units; J += (|D| - J)/16. */
    pj_get_timestamp(&now);
    pj_get_timestamp_freq(&freq);
    arrival = (pj_int64_t)now.u64 * s->clock_rate / (pj_int64_t)freq.u64;
    transit = (pj_int32_t)(arrival - (pj_int32_t)rtp_ts);
    if (s->rx.transit != 0) {
        pj_int32_t d = transit - s->rx.transit;
        if (d < 0) d = -d;
        s->rx.jitter += d - (pj_int32_t)(s->rx.jitter >> 4);
    }
    s->rx.transit = transit;
}

/* ---- byte-order helpers ---- */
static pj_uint32_t rd32(const pj_uint8_t *p)
{
    return ((pj_uint32_t)p[0] << 24) | ((pj_uint32_t)p[1] << 16) |
           ((pj_uint32_t)p[2] << 8) | p[3];
}

static void wr32(pj_uint8_t *p, pj_uint32_t v)
{
    p[0] = (pj_uint8_t)(v >> 24);
    p[1] = (pj_uint8_t)(v >> 16);
    p[2] = (pj_uint8_t)(v >> 8);
    p[3] = (pj_uint8_t)v;
}

unsigned rtcp_build_report(rtcp_session *s, pj_uint8_t *buf, unsigned cap)
{
    pj_uint8_t *p = buf;
    pj_uint32_t expected, lost, fraction;
    pj_uint32_t expected_interval, received_interval, lost_interval;
    pj_uint32_t now_tick;
    pj_timestamp now;
    unsigned cname_len;

    pj_assert(cap >= 64);

    pj_get_timestamp(&now);
    now_tick = now.u32.lo;

    /* ---- Sender Report (PT=200), RC=1, length=6+6=12 ---- */
    p[0] = 0x80 | 1;              /* V=2, P=0, RC=1 */
    p[1] = RTCP_PT_SR;
    p[2] = 0; p[3] = 12;          /* 12 words (SR 6 + report block 6) */
    wr32(p + 4, s->ssrc);
    wr32(p + 8, now_tick);        /* "NTP" hi = tick (reference clock) */
    wr32(p + 12, 0);              /* "NTP" lo */
    wr32(p + 16, 0);              /* RTP timestamp (not needed here)   */
    wr32(p + 20, s->tx.pkt_count);
    wr32(p + 24, s->tx.octet_count);
    p += 28;

    /* ---- reception report block ---- */
    expected = rtcp_expected(s);
    lost = (expected > s->rx.received) ? expected - s->rx.received : 0;
    expected_interval = expected - s->rx.exp_prior;
    received_interval = s->rx.received - s->rx.received_prior;
    lost_interval = expected_interval - received_interval;
    fraction = expected_interval ? (lost_interval << 8) / expected_interval : 0;
    if (fraction > 255) fraction = 255;
    s->rx.exp_prior = expected;
    s->rx.received_prior = s->rx.received;

    wr32(p, s->peer_ssrc ? s->peer_ssrc : s->ssrc); /* SSRC_1 */
    p[4] = (pj_uint8_t)fraction;    /* fraction lost (x256) */
    p[5] = (pj_uint8_t)((lost >> 16) & 0xFF);  /* cumulative lost (24b) */
    p[6] = (pj_uint8_t)((lost >> 8) & 0xFF);
    p[7] = (pj_uint8_t)(lost & 0xFF);
    wr32(p + 8, s->rx.max_seq);     /* extended highest seq */
    wr32(p + 12, s->rx.jitter >> 4);/* interarrival jitter (scaled) */
    wr32(p + 16, s->rx_lsr);        /* LSR = last peer SR tick */
    wr32(p + 20, now_tick - s->rx_lsr_tick); /* DLSR (tick units) */
    p += 24;

    /* ---- SDES (PT=202), SC=1, CNAME ---- */
    cname_len = s->cname ? (unsigned)strlen(s->cname) : 0;
    if (cname_len > 255) cname_len = 255;
    {
        unsigned sdes_len = 1 + 1 + 2 + cname_len;      /* SSRC + item */
        unsigned pad = (4 - (sdes_len & 3)) & 3;
        unsigned total = sdes_len + pad;
        p[0] = 0x80 | 1;            /* V=2, P=0, SC=1 */
        p[1] = RTCP_PT_SDES;
        p[2] = (pj_uint8_t)((total >> 2) - 1);  /* length in words - 1 */
        p[3] = (pj_uint8_t)(((total >> 2) - 1) & 0xFF);
        wr32(p + 4, s->ssrc);
        p[8] = RTCP_SDES_CNAME;     /* item type CNAME */
        p[9] = (pj_uint8_t)cname_len;
        if (cname_len) memcpy(p + 10, s->cname, cname_len);
        p += 8 + 2 + cname_len;
        while (pad--) { *p++ = 0; } /* zero padding to 32-bit */
    }

    return (unsigned)(p - buf);
}

/* Parse one reception report block (24 bytes). */
static void parse_report_block(rtcp_session *s, const pj_uint8_t *blk,
                               pj_uint32_t now_tick)
{
    pj_uint32_t src = rd32(blk);
    pj_uint32_t lsr = rd32(blk + 16);
    pj_uint32_t dlsr = rd32(blk + 20);
    pj_int32_t rtt;

    /* If this block describes OUR stream, record the peer's loss report. */
    if (s->peer_ssrc && src == s->ssrc) {
        s->peer_fraction_lost = blk[4];
        s->peer_cum_lost = ((pj_uint32_t)blk[5] << 16) |
                           ((pj_uint32_t)blk[6] << 8) | blk[7];
        s->peer_rx_pkt = 0; /* peer reports loss, not received count */
    }

    /* RTT = A - LSR - DLSR (RFC 3550 §6.4.1), tick units. */
    if (lsr) {
        rtt = (pj_int32_t)(now_tick - lsr) - (pj_int32_t)dlsr;
        if (rtt < 0) rtt = 0;
        s->rtt_tick = (pj_uint32_t)rtt;
    }
}

void rtcp_parse(rtcp_session *s, const pj_uint8_t *pkt, unsigned len)
{
    pj_timestamp now;

    pj_get_timestamp(&now);
    while (len >= 4) {
        unsigned pt, rc, length;
        const pj_uint8_t *body;

        rc = pkt[0] & 0x1F;
        pt = pkt[1];
        length = (((unsigned)pkt[2] << 8) | pkt[3]) + 1; /* words */
        length *= 4;
        if (length > len || length < 4)
            break;
        body = pkt + 4;
        len -= length;
        pkt += length;

        switch (pt) {
        case RTCP_PT_SR:
            /* sender info: SSRC + NTP(8) + RTPts + pkt + octet */
            if (length >= 24 + 24 * rc) {
                pj_uint32_t ntp_hi = rd32(body + 4);
                /* remember last peer SR (tick) for LSR/DLSR next time */
                if (ntp_hi) {
                    s->rx_lsr = ntp_hi;
                    s->rx_lsr_tick = now.u32.lo;
                }
                /* reception report blocks follow after 24-byte sender info */
                {
                    unsigned i;
                    for (i = 0; i < rc; i++) {
                        parse_report_block(s, body + 24 + i * 24, now.u32.lo);
                    }
                }
            }
            break;
        case RTCP_PT_RR:
            /* SSRC + reception report blocks */
            if (length >= 4 + 24 * rc) {
                unsigned i;
                for (i = 0; i < rc; i++) {
                    parse_report_block(s, body + 4 + i * 24, now.u32.lo);
                }
            }
            break;
        case RTCP_PT_SDES:
        case RTCP_PT_BYE:
        default:
            /* not used in this test; skip */
            break;
        }
    }
}
