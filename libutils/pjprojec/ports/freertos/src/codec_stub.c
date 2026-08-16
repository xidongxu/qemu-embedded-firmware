/*
 * codec_stub.c - minimal codec-manager stubs for the trimmed pjmedia port.
 *
 * The SDP negotiator (pjmedia/src/pjmedia/sdp_neg.c) needs a few
 * codec-manager symbols.  On this embedded build there is no real codec
 * manager (no audio/video stream engines), so we provide stubs that report
 * an empty codec list / no-format info.  This keeps INVITE/SDP negotiation
 * working for signaling tests without pulling in the media framework.
 */
#include <pjmedia/codec.h>
#include <pjmedia/vid_codec.h>
#include <pjmedia/stream_common.h>
#include <pj/pool.h>
#include <pj/errno.h>
#include <pj/string.h>
#include <stdio.h>

/* No codecs are registered on this build. */
pj_status_t pjmedia_codec_mgr_get_dyn_codecs(pjmedia_codec_mgr *mgr,
                                             pj_int8_t *count,
                                             pj_str_t dyn_codecs[])
{
    PJ_UNUSED_ARG(mgr);
    PJ_UNUSED_ARG(dyn_codecs);
    if (count)
        *count = 0;
    return PJ_SUCCESS;
}

/* Nothing to find: report not-found, insertion position = end. */
int pjmedia_codec_mgr_find_codec(const pj_str_t dyn_codecs[],
                                 unsigned count,
                                 const pj_str_t *codec,
                                 pj_bool_t *found)
{
    PJ_UNUSED_ARG(dyn_codecs);
    PJ_UNUSED_ARG(codec);
    if (found)
        *found = PJ_FALSE;
    return (int)count;
}

/* Nothing to insert. */
void pjmedia_codec_mgr_insert_codec(pj_pool_t *pool, pj_str_t dyn_codecs[],
                                    unsigned *count, const pj_str_t *codec)
{
    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(dyn_codecs);
    PJ_UNUSED_ARG(count);
    PJ_UNUSED_ARG(codec);
}

/* Format a codec id like "PCMU/8000/1". */
char *pjmedia_codec_info_to_id(const pjmedia_codec_info *info,
                               char *id, unsigned max_len)
{
    pj_ansi_snprintf(id, max_len, "%.*s/%d/%d",
                     (int)info->encoding_name.slen,
                     info->encoding_name.ptr,
                     (int)info->clock_rate,
                     (int)info->channel_cnt);
    return id;
}

/* No redundant-audio (RFC 2198) support here. */
pj_status_t pjmedia_stream_info_parse_fmtp(pj_pool_t *pool,
                                           const pjmedia_sdp_media *m,
                                           unsigned pt,
                                           pjmedia_codec_fmtp *fmtp)
{
    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(m);
    PJ_UNUSED_ARG(pt);
    PJ_UNUSED_ARG(fmtp);
    return PJ_ENOTSUP;
}

/* No video codecs on this build. */
pj_status_t pjmedia_vid_codec_mgr_get_dyn_codecs(pjmedia_vid_codec_mgr *mgr,
                                                 pj_int8_t *count,
                                                 pj_str_t dyn_codecs[])
{
    PJ_UNUSED_ARG(mgr);
    PJ_UNUSED_ARG(dyn_codecs);
    if (count)
        *count = 0;
    return PJ_SUCCESS;
}
