/*
 * pj_media_full_test.c - full-pjmedia framework self-test (stage 14).
 *
 * Now that the pjmedia media stack is linked in (endpoint / codec manager /
 * G.711 / event / RTCP / stream / conference...), verify the real framework
 * boots and the built-in codec actually encodes/decodes:
 *   1. pjmedia_endpt_create/destroy          (endpoint lifecycle)
 *   2. pjmedia_codec_g711_init + codec mgr   (register + find PCMU)
 *   3. codec alloc/open/encode/decode        (1 kHz sine -> ulaw -> back)
 *   4. event manager                         (endpt owns one)
 *   5. pjmedia_rtcp_session init + stats     (tx/rx feed, loss/jitter)
 */

#include <stdio.h>
#include <string.h>
#include "printf.h"
#include "pj_media_full_test.h"

#include <pj/pool.h>
#include <pj/os.h>
#include <pj/log.h>
#include <pjmedia/endpoint.h>
#include <pjmedia/codec.h>
#include <pjmedia/g711.h>
#include <pjmedia/event.h>
#include <pjmedia/rtcp.h>

/* lwIP compat sockets define close -> lwip_close; the pjmedia_codec_op
 * member is named "close" and must NOT be macro-expanded. */
#ifdef close
#undef close
#endif

#define SINE_AMP    16000
#define FRAME_SAMP  80          /* 10 ms @ 8 kHz */
#define FRAME_BYTES (FRAME_SAMP * 2)

int pj_media_full_test_run(void)
{
    pj_caching_pool cp;
    pjmedia_endpt *endpt = NULL;
    pjmedia_codec_mgr *cm = NULL;
    pjmedia_codec *codec = NULL;
    const pjmedia_codec_info *info[4];
    pjmedia_codec_param param;
    pjmedia_rtcp_session rtcp;
    pj_str_t codec_id;
    pj_status_t rc;
    unsigned i, cnt;
    int pass = 1;

    printf("\r\n=== PJMEDIA full framework test ===\r\n");

    rc = pj_init();
    if (rc != PJ_SUCCESS) { printf("pjmedia_full: pj_init failed (%d)\r\n", rc); return -1; }
    pj_caching_pool_init(&cp, &pj_pool_factory_default_policy, 0);

    /* 1) endpoint */
    rc = pjmedia_endpt_create(&cp.factory, NULL, 0, &endpt);
    if (rc != PJ_SUCCESS) {
        printf("pjmedia_full: endpt create FAILED (%d)\r\n", rc);
        pass = 0;
        goto done;
    }
    printf("pjmedia_full: endpt create OK (%p)\r\n", (void*)endpt);

    /* 2) codec manager + register built-in G.711 */
    cm = pjmedia_endpt_get_codec_mgr(endpt);
    if (!cm) { printf("pjmedia_full: codec mgr NULL\r\n"); pass = 0; goto done; }
    rc = pjmedia_codec_g711_init(endpt);
    if (rc != PJ_SUCCESS) { printf("pjmedia_full: g711 init FAILED (%d)\r\n", rc); pass = 0; goto done; }
    printf("pjmedia_full: codec mgr + G.711 registered\r\n");

    codec_id = pj_str("PCMU/8000/1");
    cnt = 4;
    rc = pjmedia_codec_mgr_find_codecs_by_id(cm, &codec_id, &cnt, info, NULL);
    if (rc != PJ_SUCCESS || cnt == 0) {
        printf("pjmedia_full: find PCMU FAILED (rc=%d cnt=%u)\r\n", rc, cnt);
        pass = 0;
        goto done;
    }
    printf("pjmedia_full: found %u codec(s), first = %s\r\n", cnt,
           pjmedia_codec_info_to_id(info[0], (char[32]){0}, 32));

    /* 3) alloc + open + encode + decode 1 kHz sine */
    rc = pjmedia_codec_mgr_alloc_codec(cm, info[0], &codec);
    if (rc != PJ_SUCCESS) { printf("pjmedia_full: alloc codec FAILED (%d)\r\n", rc); pass = 0; goto done; }
    rc = pjmedia_codec_mgr_get_default_param(cm, info[0], &param);
    if (rc != PJ_SUCCESS) { printf("pjmedia_full: get_default_param FAILED (%d)\r\n", rc); pass = 0; goto done; }
    rc = codec->op->open(codec, &param);
    if (rc != PJ_SUCCESS) { printf("pjmedia_full: codec open FAILED (%d)\r\n", rc); pass = 0; goto done; }
    {
        short in[FRAME_SAMP];
        pj_uint8_t enc[FRAME_SAMP], dec[FRAME_SAMP * 2];
        pjmedia_frame fin, fout;
        pj_timestamp ts0;
        pj_int32_t peak = 0;

        /* Fixed-point 1 kHz sine @ 8 kHz (period = 8 samples), no libm. */
        {
            static const short sine8[8] = {0, 11314, 16000, 11314,
                                           0, -11314, -16000, -11314};
            for (i = 0; i < FRAME_SAMP; ++i)
                in[i] = sine8[i & 7];
        }
        pj_bzero(&ts0, sizeof(ts0));

        fin.type = PJMEDIA_FRAME_TYPE_AUDIO;
        fin.buf = in; fin.size = sizeof(in); fin.timestamp = ts0;
        fout.buf = enc; fout.size = sizeof(enc);
        rc = codec->op->encode(codec, &fin, sizeof(enc), &fout);
        if (rc != PJ_SUCCESS || fout.size != FRAME_SAMP) {
            printf("pjmedia_full: encode FAILED (rc=%d sz=%d)\r\n", rc, (int)fout.size);
            pass = 0; goto done;
        }

        fin.type = PJMEDIA_FRAME_TYPE_AUDIO;
        fin.buf = enc; fin.size = fout.size; fin.timestamp = ts0;
        fout.buf = dec; fout.size = sizeof(dec);
        rc = codec->op->decode(codec, &fin, sizeof(dec), &fout);
        if (rc != PJ_SUCCESS || fout.size != FRAME_BYTES) {
            printf("pjmedia_full: decode FAILED (rc=%d sz=%d)\r\n", rc, (int)fout.size);
            pass = 0; goto done;
        }
        for (i = 0; i < FRAME_SAMP; ++i) {
            short s;
            memcpy(&s, dec + i * 2, 2);
            if (s < 0) s = (short)(-s);
            if (s > peak) peak = s;
        }
        printf("pjmedia_full: PCMU encode=%uB decode=%uB peak=%ld (amp %d)\r\n",
               (unsigned)fout.size, (unsigned)fout.size, (long)peak, SINE_AMP);
        if (peak < SINE_AMP * 3 / 4) {   /* G.711 quantizes but stays close */
            printf("pjmedia_full: decode peak too low -> FAIL\r\n");
            pass = 0;
        }
        codec->op->close(codec);
    }
    pjmedia_codec_mgr_dealloc_codec(cm, codec);

    /* 4) event manager */
    {
        pj_pool_t *epool = pjmedia_endpt_create_pool(endpt, "evt", 512, 512);
        pjmedia_event_mgr *em = NULL;
        rc = pjmedia_event_mgr_create(epool, 0, &em);
        printf("pjmedia_full: event mgr create %s\r\n",
               rc == PJ_SUCCESS ? "OK" : "FAILED");
        if (rc != PJ_SUCCESS || !em) pass = 0;
        if (em) pjmedia_event_mgr_destroy(em);
    }

    /* 5) RTCP session + stats */
    {
        pjmedia_rtcp_session_setting st;
        pjmedia_rtcp_session_setting_default(&st);
        st.name = "full-test";
        st.clock_rate = 8000;
        st.samples_per_frame = FRAME_SAMP;
        st.ssrc = 0x12345678;
        pjmedia_rtcp_init2(&rtcp, &st);
        for (i = 0; i < 100; ++i) {
            pjmedia_rtcp_tx_rtp(&rtcp, FRAME_SAMP);
            pjmedia_rtcp_rx_rtp(&rtcp, i, i * FRAME_SAMP, FRAME_SAMP);
        }
        printf("pjmedia_full: rtcp tx_pkt=%u tx_bytes=%u rx_pkt=%u loss=%u\r\n",
               rtcp.stat.tx.pkt, rtcp.stat.tx.bytes,
               rtcp.stat.rx.pkt, rtcp.stat.rx.loss);
        if (rtcp.stat.tx.pkt != 100 || rtcp.stat.rx.pkt != 100)
            pass = 0;
        pjmedia_rtcp_fini(&rtcp);
    }

done:
    if (endpt)
        pjmedia_endpt_destroy(endpt);
    pj_caching_pool_destroy(&cp);
    pj_shutdown();
    printf("pjmedia_full: %s\r\n", pass ? "ALL PASSED" : "FAILED");
    return pass ? 0 : -1;
}
