/*
 * audiodev_stub.c - audio-device subsystem stubs.
 *
 * pjmedia_endpt_create() is an inline that unconditionally calls
 * pjmedia_aud_subsys_init()/shutdown() (kept inline so pjmedia core does
 * not depend on the pjmedia-audiodev library).  This embedded target has
 * PJMEDIA_HAS_AUDIODEV=0 (no pjmedia-audiodev), so both are no-ops; the
 * board's own mpsx-mic/mpsx-audio drivers are used instead.
 */
#include <pj/types.h>

PJ_DEF(pj_status_t) pjmedia_aud_subsys_init(pj_pool_factory *pf)
{
    PJ_UNUSED_ARG(pf);
    return PJ_SUCCESS;
}

PJ_DEF(void) pjmedia_aud_subsys_shutdown(void)
{
}
