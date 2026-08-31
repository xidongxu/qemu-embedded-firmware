/*
 * mpsx_dev.c - pjmedia-audiodev backend driving the QEMU mpsx audio/mic
 *              devices (mps2-an505) as a real sound device for pjsua.
 *
 * This is the embedded "sound card" for the PJSUA phone app:
 *   - playback : mpsx-simple-audio (0x51002000, IRQ 49) streams S16 PCM out
 *                of a guest RAM buffer in a continuous loop
 *   - capture  : mpsx-simple-mic   (0x51003000, IRQ 50) DMA-writes captured
 *                PCM into a guest RAM buffer in a continuous loop
 *
 * The mpsx devices are "whole-buffer round" DMA engines: they set STATUS.DONE
 * (and raise an IRQ) each time one BUF_LEN buffer has been consumed/filled,
 * then keep looping.  By sizing BUF_LEN to exactly one pjsua frame (20ms),
 * each DONE interrupt corresponds to one pjmedia frame, which is used to
 * drive the pjmedia_aud_play_cb / pjmedia_aud_rec_cb callbacks.
 *
 * Two FreeRTOS tasks (one for playback, one for capture) wait on semaphores
 * that the IRQ handlers give via the weak hooks audio_done_hook() /
 * mic_done_hook() (defined in audio.c / mic.c, overridden here).
 *
 * Registering: pjmedia_aud_subsys_init() (in pjmedia-audiodev/audiodev.c)
 * registers this factory when PJMEDIA_AUDIO_DEV_HAS_MPSX is defined in
 * config_site.h.  The application then calls pjsua_set_snd_dev() with the
 * device id from pjmedia_aud_dev_lookup("mpsx", ...).
 */
#include <pjmedia-audiodev/audiodev_imp.h>
#include <pj/assert.h>
#include <pj/log.h>
#include <pj/os.h>

#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>

#include "ARMCM33_DSP_FP.h"
#include "audio.h"
#include "mic.h"
#include "mpsx_dev.h"

#if PJMEDIA_AUDIO_DEV_HAS_MPSX

#define THIS_FILE               "mpsx_dev.c"

#define MPSX_CLOCK_RATE         8000    /* default 8 kHz                   */
#define MPSX_CHANNELS           1
#define MPSX_FRAME_MS           20      /* pjmedia default ptime           */
#define MPSX_SAMPLES_PER_FRAME  (MPSX_CLOCK_RATE * MPSX_FRAME_MS / 1000) /*160*/
#define MPSX_FRAME_BYTES        (MPSX_SAMPLES_PER_FRAME * 2)  /* S16 mono */
/* Maximum supported rate / frame capacity (QEMU device accepts up to
 * 192 kHz; buffers are sized for the largest frame we expect: 48 kHz @
 * 20 ms = 960 samples). */
#define MPSX_MAX_CLOCK_RATE         48000
#define MPSX_MAX_SAMPLES_PER_FRAME  (MPSX_MAX_CLOCK_RATE * MPSX_FRAME_MS / 1000)
#define MPSX_MAX_FRAME_BYTES        (MPSX_MAX_SAMPLES_PER_FRAME * 2)
#define MPSX_TASK_PRIO          3
/* Task stack in WORDS (xTaskCreate units).  Must cover the deepest media
 * call chain: pjsua snd callback -> conf bridge -> stream put_frame ->
 * codec encode -> opus_encode -> silk (48k resampling uses a ~2 KB dynamic
 * alloca).  The 48k Opus path under pjsua needs well over 16 KB (measured:
 * still HardFaulted at sub sp,sp,r2 with a 4096-word stack); 8192 words is
 * the safe headroom for real-HW too. */
#define MPSX_TASK_STACK         8192

/* Device info */
struct mpsx_dev_info
{
    pjmedia_aud_dev_info         info;
    unsigned                     dev_id;
};

/* Factory */
struct mpsx_factory
{
    pjmedia_aud_dev_factory      base;
    pj_pool_t                   *pool;
    pj_pool_factory             *pf;
    unsigned                     dev_count;
    struct mpsx_dev_info        *dev_info;
};

/* Sound stream */
struct mpsx_stream
{
    pjmedia_aud_stream           base;
    pjmedia_aud_param            param;
    pj_pool_t                   *pool;

    pjmedia_aud_rec_cb           rec_cb;
    pjmedia_aud_play_cb          play_cb;
    void                        *user_data;

    /* FreeRTOS driver state */
    TaskHandle_t                 play_task;
    TaskHandle_t                 cap_task;
    SemaphoreHandle_t            play_sem;
    SemaphoreHandle_t            cap_sem;
    volatile int                 running;

    /* Frame timestamp tracking (monotonic sample counters) */
    pj_uint64_t                  ts_play;
    pj_uint64_t                  ts_cap;

    /* Runtime clock / frame geometry (from pjmedia_aud_param.clock_rate) */
    unsigned                     clock_rate;
    unsigned                     samples_per_frame;
    unsigned                     frame_bytes;

    /* DMA buffers (physical == virtual on MPS2 RAM); the audio device
     * loops play_buf, the mic device fills cap_buf. */
    int16_t                      play_buf[MPSX_MAX_SAMPLES_PER_FRAME]
                                 __attribute__((aligned(64)));
    int16_t                      cap_buf[MPSX_MAX_SAMPLES_PER_FRAME]
                                 __attribute__((aligned(64)));
};

/* Single active stream (pjsua uses one sound device).  Accessed from ISR
 * context, so it must be volatile. */
static volatile struct mpsx_stream *s_stream = NULL;

/* Prototypes */
static pj_status_t mpsx_factory_init(pjmedia_aud_dev_factory *f);
static pj_status_t mpsx_factory_destroy(pjmedia_aud_dev_factory *f);
static pj_status_t mpsx_factory_refresh(pjmedia_aud_dev_factory *f);
static unsigned    mpsx_factory_get_dev_count(pjmedia_aud_dev_factory *f);
static pj_status_t mpsx_factory_get_dev_info(pjmedia_aud_dev_factory *f,
                                             unsigned index,
                                             pjmedia_aud_dev_info *info);
static pj_status_t mpsx_factory_default_param(pjmedia_aud_dev_factory *f,
                                              unsigned index,
                                              pjmedia_aud_param *param);
static pj_status_t mpsx_factory_create_stream(pjmedia_aud_dev_factory *f,
                                              const pjmedia_aud_param *param,
                                              pjmedia_aud_rec_cb rec_cb,
                                              pjmedia_aud_play_cb play_cb,
                                              void *user_data,
                                              pjmedia_aud_stream **p_aud_strm);

static pj_status_t mpsx_stream_get_param(pjmedia_aud_stream *strm,
                                         pjmedia_aud_param *param);
static pj_status_t mpsx_stream_get_cap(pjmedia_aud_stream *strm,
                                       pjmedia_aud_dev_cap cap,
                                       void *value);
static pj_status_t mpsx_stream_set_cap(pjmedia_aud_stream *strm,
                                       pjmedia_aud_dev_cap cap,
                                       const void *value);
static pj_status_t mpsx_stream_start(pjmedia_aud_stream *strm);
static pj_status_t mpsx_stream_stop(pjmedia_aud_stream *strm);
static pj_status_t mpsx_stream_destroy(pjmedia_aud_stream *strm);

/* Operations */
static pjmedia_aud_dev_factory_op factory_op =
{
    &mpsx_factory_init,
    &mpsx_factory_destroy,
    &mpsx_factory_get_dev_count,
    &mpsx_factory_get_dev_info,
    &mpsx_factory_default_param,
    &mpsx_factory_create_stream,
    &mpsx_factory_refresh
};

static pjmedia_aud_stream_op stream_op =
{
    &mpsx_stream_get_param,
    &mpsx_stream_get_cap,
    &mpsx_stream_set_cap,
    &mpsx_stream_start,
    &mpsx_stream_stop,
    &mpsx_stream_destroy
};

/* ISR hooks (weak in audio.c / mic.c) - give the per-direction semaphore. */
void audio_done_hook(void)
{
    struct mpsx_stream *strm = (struct mpsx_stream *)s_stream;
    BaseType_t hp = pdFALSE;

    if (strm && strm->play_sem) {
        xSemaphoreGiveFromISR(strm->play_sem, &hp);
        portYIELD_FROM_ISR(hp);
    }
}

void mic_done_hook(void)
{
    struct mpsx_stream *strm = (struct mpsx_stream *)s_stream;
    BaseType_t hp = pdFALSE;

    if (strm && strm->cap_sem) {
        xSemaphoreGiveFromISR(strm->cap_sem, &hp);
        portYIELD_FROM_ISR(hp);
    }
}

/* Playback task: on each audio DONE, pull one frame from pjsua into the
 * DMA buffer that the device keeps looping. */
static void mpsx_play_task(void *arg)
{
    struct mpsx_stream *strm = (struct mpsx_stream *)arg;
    pjmedia_frame f;

    f.type = PJMEDIA_FRAME_TYPE_AUDIO;
    f.size = strm->frame_bytes;

    while (strm->running) {
        if (xSemaphoreTake(strm->play_sem, pdMS_TO_TICKS(200)) != pdTRUE)
            continue;
        f.buf = strm->play_buf;
        f.timestamp.u64 = strm->ts_play;
        strm->ts_play += strm->samples_per_frame;
        if (strm->play_cb)
            strm->play_cb(strm->user_data, &f);
        /* play_buf content updated in place; device loops the same buffer */
    }
    vTaskDelete(NULL);
}

/* Capture task: on each mic DONE, hand the freshly DMA-written frame to
 * pjsua via the rec callback. */
static void mpsx_cap_task(void *arg)
{
    struct mpsx_stream *strm = (struct mpsx_stream *)arg;
    pjmedia_frame f;

    f.type = PJMEDIA_FRAME_TYPE_AUDIO;
    f.size = strm->frame_bytes;

    while (strm->running) {
        if (xSemaphoreTake(strm->cap_sem, pdMS_TO_TICKS(200)) != pdTRUE)
            continue;
        f.buf = strm->cap_buf;   /* mic DMA wrote this buffer */
        f.timestamp.u64 = strm->ts_cap;
        strm->ts_cap += strm->samples_per_frame;
        if (strm->rec_cb) {
            strm->rec_cb(strm->user_data, &f);
        }
    }
    vTaskDelete(NULL);
}

/****************************************************************************
 * Factory operations
 */
pjmedia_aud_dev_factory* pjmedia_mpsx_audio_factory(pj_pool_factory *pf)
{
    struct mpsx_factory *f;
    pj_pool_t *pool;

    pool = pj_pool_create(pf, "mpsx audio", 1000, 1000, NULL);
    f = PJ_POOL_ZALLOC_T(pool, struct mpsx_factory);
    f->pf = pf;
    f->pool = pool;
    f->base.op = &factory_op;

    return &f->base;
}

static pj_status_t mpsx_factory_init(pjmedia_aud_dev_factory *f)
{
    struct mpsx_factory *mf = (struct mpsx_factory *)f;
    struct mpsx_dev_info *mdi;

    mf->dev_count = 1;
    mf->dev_info = (struct mpsx_dev_info *)
                   pj_pool_calloc(mf->pool, mf->dev_count,
                                  sizeof(struct mpsx_dev_info));
    mdi = &mf->dev_info[0];
    pj_bzero(mdi, sizeof(*mdi));
    pj_ansi_strxcpy(mdi->info.name, "mpsx audio/mic",
                    sizeof(mdi->info.name));
    pj_ansi_strxcpy(mdi->info.driver, "mpsx", sizeof(mdi->info.driver));
    mdi->info.input_count = 1;
    mdi->info.output_count = 1;
    mdi->info.default_samples_per_sec = MPSX_CLOCK_RATE;
    mdi->info.caps = 0;

    PJ_LOG(4, (THIS_FILE, "mpsx audio/mic initialized"));

    return PJ_SUCCESS;
}

static pj_status_t mpsx_factory_destroy(pjmedia_aud_dev_factory *f)
{
    struct mpsx_factory *mf = (struct mpsx_factory *)f;

    pj_pool_safe_release(&mf->pool);
    return PJ_SUCCESS;
}

static pj_status_t mpsx_factory_refresh(pjmedia_aud_dev_factory *f)
{
    PJ_UNUSED_ARG(f);
    return PJ_SUCCESS;
}

static unsigned mpsx_factory_get_dev_count(pjmedia_aud_dev_factory *f)
{
    struct mpsx_factory *mf = (struct mpsx_factory *)f;
    return mf->dev_count;
}

static pj_status_t mpsx_factory_get_dev_info(pjmedia_aud_dev_factory *f,
                                             unsigned index,
                                             pjmedia_aud_dev_info *info)
{
    struct mpsx_factory *mf = (struct mpsx_factory *)f;

    PJ_ASSERT_RETURN(index < mf->dev_count, PJMEDIA_EAUD_INVDEV);
    pj_memcpy(info, &mf->dev_info[index].info, sizeof(*info));
    return PJ_SUCCESS;
}

static pj_status_t mpsx_factory_default_param(pjmedia_aud_dev_factory *f,
                                              unsigned index,
                                              pjmedia_aud_param *param)
{
    struct mpsx_factory *mf = (struct mpsx_factory *)f;
    struct mpsx_dev_info *di = &mf->dev_info[index];

    PJ_ASSERT_RETURN(index < mf->dev_count, PJMEDIA_EAUD_INVDEV);

    pj_bzero(param, sizeof(*param));
    if (di->info.input_count && di->info.output_count) {
        param->dir = PJMEDIA_DIR_CAPTURE_PLAYBACK;
        param->rec_id = index;
        param->play_id = index;
    } else if (di->info.input_count) {
        param->dir = PJMEDIA_DIR_CAPTURE;
        param->rec_id = index;
        param->play_id = PJMEDIA_AUD_INVALID_DEV;
    } else if (di->info.output_count) {
        param->dir = PJMEDIA_DIR_PLAYBACK;
        param->play_id = index;
        param->rec_id = PJMEDIA_AUD_INVALID_DEV;
    } else {
        return PJMEDIA_EAUD_INVDEV;
    }

    param->clock_rate = MPSX_CLOCK_RATE;
    param->channel_count = MPSX_CHANNELS;
    param->samples_per_frame = MPSX_SAMPLES_PER_FRAME;
    param->bits_per_sample = 16;
    param->flags = 0;

    return PJ_SUCCESS;
}

static pj_status_t mpsx_factory_create_stream(pjmedia_aud_dev_factory *f,
                                              const pjmedia_aud_param *param,
                                              pjmedia_aud_rec_cb rec_cb,
                                              pjmedia_aud_play_cb play_cb,
                                              void *user_data,
                                              pjmedia_aud_stream **p_aud_strm)
{
    struct mpsx_factory *mf = (struct mpsx_factory *)f;
    pj_pool_t *pool;
    struct mpsx_stream *strm;

    pool = pj_pool_create(mf->pf, "mpsx-dev", 1000, 1000, NULL);
    PJ_ASSERT_RETURN(pool != NULL, PJ_ENOMEM);

    strm = PJ_POOL_ZALLOC_T(pool, struct mpsx_stream);
    pj_memcpy(&strm->param, param, sizeof(*param));
    strm->pool = pool;
    strm->rec_cb = rec_cb;
    strm->play_cb = play_cb;
    strm->user_data = user_data;

    /* Runtime clock geometry from the requested sample rate. */
    strm->clock_rate = param->clock_rate;
    strm->samples_per_frame = param->clock_rate * MPSX_FRAME_MS / 1000;
    strm->frame_bytes = strm->samples_per_frame * 2;

    strm->base.op = &stream_op;
    *p_aud_strm = &strm->base;

    /* Remember the active stream for the ISR hooks (single-device model). */
    s_stream = strm;

    return PJ_SUCCESS;
}

/****************************************************************************
 * Stream operations
 */
static pj_status_t mpsx_stream_get_param(pjmedia_aud_stream *s,
                                         pjmedia_aud_param *pi)
{
    struct mpsx_stream *strm = (struct mpsx_stream *)s;

    PJ_ASSERT_RETURN(strm && pi, PJ_EINVAL);
    pj_memcpy(pi, &strm->param, sizeof(*pi));
    return PJ_SUCCESS;
}

static pj_status_t mpsx_stream_get_cap(pjmedia_aud_stream *s,
                                       pjmedia_aud_dev_cap cap,
                                       void *pval)
{
    PJ_UNUSED_ARG(s); PJ_UNUSED_ARG(pval);
    return PJMEDIA_EAUD_INVCAP;
}

static pj_status_t mpsx_stream_set_cap(pjmedia_aud_stream *s,
                                       pjmedia_aud_dev_cap cap,
                                       const void *pval)
{
    PJ_UNUSED_ARG(s); PJ_UNUSED_ARG(pval);
    return PJMEDIA_EAUD_INVCAP;
}

static pj_status_t mpsx_stream_start(pjmedia_aud_stream *s)
{
    struct mpsx_stream *strm = (struct mpsx_stream *)s;
    unsigned dir = strm->param.dir;

    if (strm->running)
        return PJ_SUCCESS;

    strm->running = 1;
    strm->ts_play = 0;
    strm->ts_cap = 0;

    /* ---- Playback: mpsx-simple-audio (IRQ 49) ---- */
    if (dir & PJMEDIA_DIR_PLAYBACK) {
        strm->play_sem = xSemaphoreCreateBinary();
        if (!strm->play_sem) {
            strm->running = 0;
            return PJ_ENOMEM;
        }
        /* silent frame first */
        pj_bzero(strm->play_buf, sizeof(strm->play_buf));

        AUDIO_CTRL = 0;
        __DSB();
        AUDIO_INT_STATUS = AUDIO_INT_DONE;
        AUDIO_INT_EN = AUDIO_INT_DONE;
        AUDIO_FORMAT = AUDIO_FORMAT_S16;
        AUDIO_SAMPLE_RATE = strm->clock_rate;
        AUDIO_BUF_ADDR = (uint32_t)(uintptr_t)strm->play_buf;
        AUDIO_BUF_LEN = strm->frame_bytes;
        NVIC_ClearPendingIRQ((IRQn_Type)AUDIO_IRQ);
        NVIC_EnableIRQ((IRQn_Type)AUDIO_IRQ);
        __DSB();
        AUDIO_CTRL = AUDIO_CTRL_ENABLE | AUDIO_CTRL_UPDATE;

        if (xTaskCreate(mpsx_play_task, "mpsx_play", MPSX_TASK_STACK,
                        strm, MPSX_TASK_PRIO, &strm->play_task) != pdPASS) {
            strm->running = 0;
            return PJ_ENOMEM;
        }
        PJ_LOG(4, (THIS_FILE, "mpsx playback started (buf=%p len=%u)",
                   (void *)strm->play_buf, (unsigned)strm->frame_bytes));
    }

    /* ---- Capture: mpsx-simple-mic (IRQ 50) ---- */
    if (dir & PJMEDIA_DIR_CAPTURE) {
        strm->cap_sem = xSemaphoreCreateBinary();
        if (!strm->cap_sem) {
            strm->running = 0;
            return PJ_ENOMEM;
        }

        MIC_CTRL = 0;
        __DSB();
        MIC_INT_STATUS = MIC_INT_DONE | MIC_INT_OVERRUN;
        MIC_INT_EN = MIC_INT_DONE;
        MIC_FORMAT = MIC_FORMAT_S16;
        MIC_SAMPLE_RATE = strm->clock_rate;
        MIC_BUF_ADDR = (uint32_t)(uintptr_t)strm->cap_buf;
        MIC_BUF_LEN = strm->frame_bytes;
        NVIC_ClearPendingIRQ((IRQn_Type)MIC_IRQ);
        NVIC_EnableIRQ((IRQn_Type)MIC_IRQ);
        __DSB();
        MIC_CTRL = MIC_CTRL_ENABLE | MIC_CTRL_UPDATE;

        if (xTaskCreate(mpsx_cap_task, "mpsx_cap", MPSX_TASK_STACK,
                        strm, MPSX_TASK_PRIO, &strm->cap_task) != pdPASS) {
            strm->running = 0;
            return PJ_ENOMEM;
        }
        PJ_LOG(4, (THIS_FILE, "mpsx capture started (buf=%p len=%u)",
                   (void *)strm->cap_buf, (unsigned)strm->frame_bytes));
    }

    return PJ_SUCCESS;
}

static pj_status_t mpsx_stream_stop(pjmedia_aud_stream *s)
{
    struct mpsx_stream *strm = (struct mpsx_stream *)s;

    if (!strm->running)
        return PJ_SUCCESS;

    strm->running = 0;

    AUDIO_CTRL = 0;
    MIC_CTRL = 0;

    if (strm->play_task) {
        vTaskDelete(strm->play_task);
        strm->play_task = NULL;
    }
    if (strm->cap_task) {
        vTaskDelete(strm->cap_task);
        strm->cap_task = NULL;
    }
    if (strm->play_sem) {
        vSemaphoreDelete(strm->play_sem);
        strm->play_sem = NULL;
    }
    if (strm->cap_sem) {
        vSemaphoreDelete(strm->cap_sem);
        strm->cap_sem = NULL;
    }

    return PJ_SUCCESS;
}

static pj_status_t mpsx_stream_destroy(pjmedia_aud_stream *s)
{
    struct mpsx_stream *strm = (struct mpsx_stream *)s;

    PJ_ASSERT_RETURN(strm != NULL, PJ_EINVAL);

    mpsx_stream_stop(s);
    if (s_stream == strm)
        s_stream = NULL;

    pj_pool_release(strm->pool);
    return PJ_SUCCESS;
}

#endif  /* PJMEDIA_AUDIO_DEV_HAS_MPSX */
