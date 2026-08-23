/*
 * audiodev_stub.c - audio-device subsystem stubs.
 *
 * The real pjmedia_aud_subsys_init()/shutdown() now come from the
 * pjmedia-audiodev library (PJMEDIA_HAS_AUDIODEV=1), so no stub is kept
 * here.  These stubs only cover the legacy pjmedia sound-port layer that
 * conference.c references at link time even though we always create the
 * bridge with PJMEDIA_CONF_NO_DEVICE (no sound device); they are never
 * actually called, they just satisfy the linker.
 */
#include <pj/types.h>
#include <pj/string.h>

/* ---- pjmedia sound-port stubs ----
 * conference.c references the sound-port layer at link time even though we
 * always create the bridge with PJMEDIA_CONF_NO_DEVICE (no sound device),
 * so these are never actually called.  They just satisfy the linker. */
#include <pjmedia/sound_port.h>
#include <pjmedia/echo.h>

PJ_DEF(pj_status_t) pjmedia_snd_port_create(pj_pool_t *pool,
                                            int rec_id, int play_id,
                                            unsigned clock_rate,
                                            unsigned channel_count,
                                            unsigned samples_per_frame,
                                            unsigned bits_per_sample,
                                            unsigned options,
                                            pjmedia_snd_port **p_port)
{
    PJ_UNUSED_ARG(pool); PJ_UNUSED_ARG(rec_id); PJ_UNUSED_ARG(play_id);
    PJ_UNUSED_ARG(clock_rate); PJ_UNUSED_ARG(channel_count);
    PJ_UNUSED_ARG(samples_per_frame); PJ_UNUSED_ARG(bits_per_sample);
    PJ_UNUSED_ARG(options); PJ_UNUSED_ARG(p_port);
    return PJ_ENOTSUP;
}

PJ_DEF(pj_status_t) pjmedia_snd_port_create_player(pj_pool_t *pool,
                                                   int index,
                                                   unsigned clock_rate,
                                                   unsigned channel_count,
                                                   unsigned samples_per_frame,
                                                   unsigned bits_per_sample,
                                                   unsigned options,
                                                   pjmedia_snd_port **p_port)
{
    PJ_UNUSED_ARG(pool); PJ_UNUSED_ARG(index);
    PJ_UNUSED_ARG(clock_rate); PJ_UNUSED_ARG(channel_count);
    PJ_UNUSED_ARG(samples_per_frame); PJ_UNUSED_ARG(bits_per_sample);
    PJ_UNUSED_ARG(options); PJ_UNUSED_ARG(p_port);
    return PJ_ENOTSUP;
}

PJ_DEF(pjmedia_aud_stream*) pjmedia_snd_port_get_snd_stream(
                                        pjmedia_snd_port *snd_port)
{
    PJ_UNUSED_ARG(snd_port);
    return NULL;
}

PJ_DEF(pj_status_t) pjmedia_snd_port_connect(pjmedia_snd_port *snd_port,
                                             pjmedia_port *sink_port)
{
    PJ_UNUSED_ARG(snd_port); PJ_UNUSED_ARG(sink_port);
    return PJ_ENOTSUP;
}

PJ_DEF(pj_status_t) pjmedia_snd_port_destroy(pjmedia_snd_port *snd_port)
{
    PJ_UNUSED_ARG(snd_port);
    return PJ_SUCCESS;
}

PJ_DEF(pj_status_t) pjmedia_snd_port_get_ec_stat(pjmedia_snd_port *snd_port,
                                                 pjmedia_echo_stat *p_stat)
{
    PJ_UNUSED_ARG(snd_port); PJ_UNUSED_ARG(p_stat);
    return PJ_ENOTSUP;
}

PJ_DEF(void) pjmedia_snd_port_param_default(pjmedia_snd_port_param *prm)
{
    pj_bzero(prm, sizeof(*prm));
}

PJ_DEF(pj_status_t) pjmedia_snd_port_create2(pj_pool_t *pool,
                                             const pjmedia_snd_port_param *prm,
                                             pjmedia_snd_port **p_port)
{
    PJ_UNUSED_ARG(pool); PJ_UNUSED_ARG(prm); PJ_UNUSED_ARG(p_port);
    return PJ_ENOTSUP;
}

PJ_DEF(pj_status_t) pjmedia_snd_port_disconnect(pjmedia_snd_port *snd_port)
{
    PJ_UNUSED_ARG(snd_port);
    return PJ_ENOTSUP;
}
