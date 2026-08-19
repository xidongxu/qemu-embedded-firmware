/*
 * codec_stub.c - video codec-manager stub for the trimmed pjmedia port.
 *
 * Audio codecs come from the real pjmedia codec manager (codec.c + g711.c).
 * Video is disabled (PJMEDIA_HAS_VIDEO=0), so only the tiny video-codec
 * query stub the SDP negotiator needs is kept here.
 */
#include <pjmedia/vid_codec.h>
#include <pj/errno.h>
#include <pj/string.h>

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
