#ifndef PJ_RTCP_ENGINE_H
#define PJ_RTCP_ENGINE_H

/*
 * Minimal RTCP engine (RFC 3550 subset) for the dual-QEMU call test.
 *
 * Implements just enough of RTCP to monitor a bidirectional RTP stream:
 *   - SR (Sender Report, PT=200) + reception report block + SDES CNAME
 *   - RR (Receiver Report, PT=201) parsing
 *   - SDES / BYE parsing
 *   - sender stats (packets/octets sent)
 *   - receiver stats: expected/lost/fraction-lost, interarrival jitter,
 *     extended highest sequence number
 *   - round-trip time (RTT) from the peer's LSR/DLSR (RFC 3550 §6.4.1)
 *
 * Reference clock: the system high-resolution timestamp (pj_get_timestamp)
 * is used as a stand-in for NTP.  The SR's "NTP" fields carry the low 32
 * bits of the current tick and LSR/DLSR are expressed in tick units, so the
 * RTT formula (A - LSR - DLSR) is self-consistent even though we are not
 * using real NTP time.
 */

#include <pj/types.h>
#include <pj/os.h>
#include <pj/math.h>

#define RTCP_PT_SR     200
#define RTCP_PT_RR     201
#define RTCP_PT_SDES   202
#define RTCP_PT_BYE    203

#define RTCP_SDES_CNAME  1

/* One-direction stream statistics (RFC 3550 §6.4.1). */
typedef struct rtcp_stream_stat
{
    pj_uint32_t base_seq;        /* first RTP seq seen               */
    pj_uint32_t max_seq;         /* highest RTP seq seen             */
    pj_uint32_t received;        /* packets received (incl. dup)     */
    pj_uint32_t received_prior;  /* received at last interval        */
    pj_uint32_t exp_prior;       /* expected at last interval        */
    pj_uint32_t jitter;          /* interarrival jitter (scaled)     */
    pj_int32_t  transit;         /* relative transit time (RTP ts)   */
    pj_uint32_t pkt_count;       /* packets sent (tx only)           */
    pj_uint32_t octet_count;     /* payload octets sent (tx only)    */
} rtcp_stream_stat;

typedef struct rtcp_session
{
    pj_uint32_t     ssrc;         /* our SSRC                        */
    unsigned        clock_rate;   /* RTP clock rate (e.g. 8000)      */
    pj_uint32_t     peer_ssrc;    /* peer SSRC (0 = unknown)         */
    const char     *cname;        /* our CNAME string                */
    rtcp_stream_stat tx;          /* our transmit stats              */
    rtcp_stream_stat rx;          /* our receive stats               */
    /* RTT measurement */
    pj_uint32_t     rx_lsr;       /* low32 NTP(tick) in last peer SR */
    pj_uint32_t     rx_lsr_tick;  /* local tick when last peer SR    */
    pj_uint32_t     rtt_tick;     /* last measured RTT (ticks)       */
    /* peer-reported receive stats (what the peer lost of ours)      */
    pj_uint32_t     peer_rx_pkt;  /* peer's received count of our pkts */
    pj_uint32_t     peer_fraction_lost; /* peer's fraction lost (x256) */
    pj_uint32_t     peer_cum_lost;      /* peer's cumulative lost     */
} rtcp_session;

/* Init the session (tx + rx stats zeroed). */
void rtcp_init(rtcp_session *s, pj_uint32_t ssrc, unsigned clock_rate,
               pj_uint32_t peer_ssrc, const char *cname);

/* Feed an outgoing RTP frame (call on every sent frame). */
void rtcp_tx_rtp(rtcp_session *s, unsigned payload_len);

/* Feed an incoming RTP frame (call on every received frame). */
void rtcp_rx_rtp(rtcp_session *s, unsigned seq, unsigned rtp_ts);

/* Build an SR (+ reception report + SDES CNAME) into buf.
 * Returns the total RTCP packet length in bytes. */
unsigned rtcp_build_report(rtcp_session *s, pj_uint8_t *buf, unsigned cap);

/* Parse an incoming RTCP compound packet (may contain SR/RR/SDES/BYE). */
void rtcp_parse(rtcp_session *s, const pj_uint8_t *pkt, unsigned len);

/* ---- derived stats (for reporting) ---- */
/* Expected packets received (RFC 3550: max_seq - base_seq + 1). */
PJ_INLINE(pj_uint32_t) rtcp_expected(const rtcp_session *s)
{
    return s->rx.max_seq - s->rx.base_seq + 1;
}

/* Cumulative lost (expected - received, clamped). */
PJ_INLINE(pj_int32_t) rtcp_cum_lost(const rtcp_session *s)
{
    pj_uint32_t e = rtcp_expected(s);
    return (e > s->rx.received) ? (pj_int32_t)(e - s->rx.received) : 0;
}

/* RTT in microseconds (0 if not measured yet). */
PJ_INLINE(pj_uint32_t) rtcp_rtt_us(const rtcp_session *s)
{
    pj_timestamp freq;
    if (!s->rtt_tick)
        return 0;
    pj_get_timestamp_freq(&freq);
    return (pj_uint32_t)((pj_uint64_t)s->rtt_tick * 1000000 / freq.u64);
}

#endif /* PJ_RTCP_ENGINE_H */
