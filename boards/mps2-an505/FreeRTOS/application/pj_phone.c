/*
 * pj_phone.c - PJSUA high-level phone application (experiment).
 *
 * Uses the pjsua high-level API (pjsua_create/init/transport/acc/call) to
 * place a SIP call to a device on the HOST (e.g. pjsua or a SIP server)
 * through QEMU slirp + hostfwd.
 *
 * Topology (slirp user-net + hostfwd):
 *   guest SIP  : bind 0.0.0.0:<GUEST_SIP_PORT>  (hostfwd udp::<port>-:<port>)
 *   dial target: sip:user@10.0.2.2:<HOST_SIP_PORT>  (gateway = host loopback)
 *   host responds to the Via "received" (127.0.0.1:<port>) -> hostfwd -> guest
 *
 * Audio: null device (embedded has no sound card).  Media RTP ports are
 * auto-assigned by pjsua, so host->guest RTP is limited under slirp; this
 * first step focuses on the high-level signalling path (register + call).
 */
#include <stdio.h>
#include <string.h>
#include "printf.h"
#include "pj_phone.h"

#include <FreeRTOS.h>
#include <task.h>

#include <pj/errno.h>
#include <pj/log.h>
#include <pj/string.h>
#include <pj/sock.h>
#include <pjmedia/audiodev.h>
#include <pjmedia-audiodev/audiodev.h>
#include <pjsua-lib/pjsua.h>
#include "mpsx_dev.h"

#define HOST_GW        "10.0.2.2"   /* slirp gateway = host loopback alias */
#define HOST_SIP_PORT  5060         /* host pjsua / SIP server UDP port */
#define GUEST_SIP_PORT 15062        /* guest SIP port (hostfwd'd as itself) */
#define DIAL_TARGET    "sip:user@10.0.2.2:5060"

static pjsua_acc_id g_acc = PJSUA_INVALID_ID;
static pjsua_call_id g_call_id = PJSUA_INVALID_ID;   /* active call for stats */

/* ---- call-control state (auto hangup + redial) ---- */
#define CALL_DURATION_MS    15000   /* auto-hangup after 15 s in a call  */
#define REDIAL_DELAY_MS      5000   /* wait 5 s after hangup, then redial */
static uint32_t    g_call_start_tick; /* tick when call media went ACTIVE  */
static uint32_t    g_last_idle_tick;  /* tick when last call disconnected  */
static int         g_call_state = PJSIP_INV_STATE_NULL;
static pj_bool_t   g_auto_redial = PJ_TRUE;

/* High-priority watchdog: keeps reporting free heap / task stack high-water
 * even if the pjsua worker thread stalls (deadlock / stack-overflow debug). */
static const char *wd_state_name(int s)
{
    switch (s) {
    case 0: return "RUN";   /* eRunning */
    case 1: return "RDY";   /* eReady */
    case 2: return "BLK";   /* eBlocked */
    case 3: return "SUS";   /* eSuspended */
    default: return "?";    /* eDeleted/eInvalid */
    }
}

void phone_watchdog(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        /* RTP stream stat for audio media (index 0). */
        if (g_call_id != PJSUA_INVALID_ID) {
            pjsua_stream_stat ss;
            if (pjsua_call_get_stream_stat(g_call_id, 0, &ss) == PJ_SUCCESS) {
                printf("wd: rx_pkt=%lu tx_pkt=%lu rx_lost=%lu\r\n",
                       (unsigned long)ss.rtcp.rx.pkt,
                       (unsigned long)ss.rtcp.tx.pkt,
                       (unsigned long)ss.rtcp.rx.loss);
            }
            /* Conference signal level of the call slot: tx = what we send
             * (mic capture), rx = what we play (from the remote). */
            {
                pjsua_call_info ci;
                if (pjsua_call_get_info(g_call_id, &ci) == PJ_SUCCESS &&
                    ci.conf_slot != PJSUA_INVALID_ID)
                {
                    unsigned tx = 0, rx = 0;
                    pjsua_conf_get_signal_level(ci.conf_slot, &tx, &rx);
                    printf("wd: conf sig tx=%u rx=%u\r\n", tx, rx);
                }
            }
        }
        TaskHandle_t cur = xTaskGetCurrentTaskHandle();
        printf("wd: current=%s\r\n", pcTaskGetName(cur));
        UBaseType_t n = uxTaskGetNumberOfTasks();
        TaskStatus_t *st = pvPortMalloc(n * sizeof(TaskStatus_t));
        if (st) {
            n = uxTaskGetSystemState(st, n, NULL);
            for (UBaseType_t i = 0; i < n; i++) {
                printf("wd: %-13s hwm=%lu st=%s\r\n", st[i].pcTaskName,
                       (unsigned long)st[i].usStackHighWaterMark,
                       wd_state_name((int)st[i].eCurrentState));
            }
            vPortFree(st);
        } else {
            printf("wd: malloc fail heap=%u\r\n",
                   (unsigned)xPortGetFreeHeapSize());
        }
    }
}

/* --------------------------- callbacks --------------------------- */
static void on_reg_state(pjsua_acc_id acc_id)
{
    pjsua_acc_info info;
    if (pjsua_acc_get_info(acc_id, &info) == PJ_SUCCESS) {
        printf("pj_phone: acc %d reg state=%d (%.*s)\r\n", acc_id,
               (int)info.status, (int)info.status_text.slen,
               info.status_text.ptr);
    }
}

static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
                             pjsip_rx_data *rdata)
{
    PJ_UNUSED_ARG(acc_id); PJ_UNUSED_ARG(rdata);
    if (g_call_id != PJSUA_INVALID_ID) {
        /* already in a call -> busy */
        printf("pj_phone: busy, rejecting incoming call %d (486)\r\n", call_id);
        pjsua_call_answer(call_id, 486, NULL, NULL);
    } else {
        printf("pj_phone: incoming call %d, answering 200\r\n", call_id);
        pjsua_call_answer(call_id, 200, NULL, NULL);
    }
}

static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    pjsua_call_info ci;
    PJ_UNUSED_ARG(e);
    if (pjsua_call_get_info(call_id, &ci) == PJ_SUCCESS) {
        g_call_state = ci.state;
        printf("pj_phone: call %d state=%d (%.*s)\r\n", call_id,
               (int)ci.state, (int)ci.state_text.slen, ci.state_text.ptr);
        if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
            printf("pj_phone: call %d disconnected, idle (redial in %u ms)\r\n",
                   call_id, (unsigned)REDIAL_DELAY_MS);
            g_call_id = PJSUA_INVALID_ID;
            g_last_idle_tick = xTaskGetTickCount();
        }
    }
}

static void on_call_media_state(pjsua_call_id call_id)
{
    pjsua_call_info ci;
    if (pjsua_call_get_info(call_id, &ci) == PJ_SUCCESS) {
        printf("pj_phone: call %d media_status=%d%s\r\n", call_id,
               (int)ci.media_status,
               ci.media_status == PJSUA_CALL_MEDIA_ACTIVE ? " (ACTIVE)" : "");
        /* Wire the call's conference slot to the sound device (slot 0) once
         * media is up, so the real mpsx audio/mic is actually used:
         *   - call -> slot 0 : incoming RTP is played on the sound card
         *   - slot 0 -> call : mic capture is sent as RTP */
        if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) {
            /* pjsua does NOT create the echo canceller automatically from
             * media_cfg.ec_tail_len; it must be enabled explicitly with
             * pjsua_set_ec() once the sound device exists (200 ms tail). */
            {
                pj_status_t e = pjsua_set_ec(200, 0);
                printf("pj_phone: pjsua_set_ec(tail=200) -> %d\r\n", (int)e);
            }
            pjsua_conf_connect(ci.conf_slot, 0);
            pjsua_conf_connect(0, ci.conf_slot);
            g_call_start_tick = xTaskGetTickCount();
            printf("pj_phone: conf connected (call slot %d <-> snd 0)\r\n",
                   ci.conf_slot);
        }
    }
}

/* --------------------------- call control ------------------------ */
static pj_status_t pj_phone_dial(void)
{
    pj_str_t uri;
    pjsua_call_id call_id;
    pj_status_t st;

    pj_strset2(&uri, DIAL_TARGET);
    st = pjsua_call_make_call(g_acc, &uri, NULL, NULL, NULL, &call_id);
    printf("pj_phone: make_call(%s) -> %d (call=%d)\r\n", DIAL_TARGET,
           (int)st, (int)call_id);
    if (st == PJ_SUCCESS)
        g_call_id = call_id;
    return st;
}

void pj_phone_control(void)
{
    uint32_t now;

    if (!g_auto_redial)
        return;

    now = xTaskGetTickCount();
    if (g_call_id != PJSUA_INVALID_ID) {
        /* In a call: report echo-canceller convergence.  The simple echo
         * suppressor exposes learning/stat_info; Speex AEC has no get_stat
         * (PJ_ENOTSUP) so nothing is printed for it. */
        {
            pjmedia_echo_stat ecs;
            if (pjsua_get_ec_stat(&ecs) == PJ_SUCCESS) {
                printf("pj_phone: ec learn=%u tail=%u min=%u avg=%u | %.*s\r\n",
                       ecs.learning, ecs.tail, ecs.min_factor, ecs.avg_factor,
                       (int)ecs.stat_info.slen, (char*)ecs.stat_info.ptr);
            }
        }
        /* in a call: auto-hangup once the call has lasted CALL_DURATION_MS */
        if ((now - g_call_start_tick) >= pdMS_TO_TICKS(CALL_DURATION_MS)) {
            printf("pj_phone: auto hangup after %u ms\r\n",
                   (unsigned)CALL_DURATION_MS);
            pjsua_call_hangup(g_call_id, 0, NULL, NULL);
            /* g_call_id cleared on DISCONNECTED in on_call_state */
        }
    } else {
        /* idle: redial once REDIAL_DELAY_MS has passed since disconnect */
        if (g_last_idle_tick != 0 &&
            (now - g_last_idle_tick) >= pdMS_TO_TICKS(REDIAL_DELAY_MS))
        {
            printf("pj_phone: redialing\r\n");
            pj_phone_dial();
            g_last_idle_tick = 0;   /* reset until the next disconnect */
        }
    }
}

/* --------------------------- entry ------------------------------- */
int pj_phone_start(void)
{
    pjsua_config cfg;
    pjsua_logging_config log_cfg;
    pjsua_media_config media_cfg;
    pjsua_transport_config tcfg;
    pjsua_acc_config acc_cfg;
    pjsua_transport_id tp;
    pj_status_t st;

    printf("\r\n=== PJSUA PHONE (high-level API) ===\r\n");

    st = pjsua_create();
    if (st != PJ_SUCCESS) {
        printf("pj_phone: pjsua_create failed (%d)\r\n", st);
        return -1;
    }

    pjsua_config_default(&cfg);
    cfg.cb.on_reg_state = &on_reg_state;
    cfg.cb.on_incoming_call = &on_incoming_call;
    cfg.cb.on_call_state = &on_call_state;
    cfg.cb.on_call_media_state = &on_call_media_state;

    pjsua_logging_config_default(&log_cfg);
    /* Level 2 = warnings+errors only.  Higher levels flood the serial port
     * with per-frame media logs which dominates guest CPU under TCG -> the
     * guest can't keep up with realtime audio. */
    log_cfg.console_level = 2;

    pjsua_media_config_default(&media_cfg);
    media_cfg.clock_rate = 8000;
    media_cfg.snd_clock_rate = 8000;
    media_cfg.channel_count = 1;
    media_cfg.ec_options = 0;
    /* The audio source is null (silence) until a real mpsx backend is wired
     * up; without this, VAD suppresses all TX packets.  Disable VAD so the
     * guest keeps sending (silence) and the RTP path stays active. */
    media_cfg.no_vad = PJ_TRUE;
    /* Default snd_auto_close_time=1 closes the (null) sound device after
     * 1s idle, which kills the media clock -> guest stops sending RTP.
     * Disable the auto-close so the guest stream keeps running. */
    media_cfg.snd_auto_close_time = -1;
    /* Use the mpsx sound device's native clock (the FreeRTOS tasks drive
     * play/rec callbacks straight into/out of the conference).  The default
     * software clock (clock_thread + cap/play delaybuf) desynchronises from
     * the mpsx DONE interrupts -> capdbuf underflow -> silence captured. */
    media_cfg.snd_use_sw_clock = PJ_FALSE;

    st = pjsua_init(&cfg, &log_cfg, &media_cfg);
    if (st != PJ_SUCCESS) {
        printf("pj_phone: pjsua_init failed (%d)\r\n", st);
        return -1;
    }
    printf("pj_phone: pjsua_init OK\r\n");

#if PJMEDIA_AUDIO_DEV_HAS_MPSX
    /* Register the mpsx audio factory at runtime using the public
     * pjmedia_aud_register_factory() API.  pjsua_init() already initialised
     * the audio subsystem, so this just appends the mpsx device to the
     * device list -- no upstream pjproject source change is needed. */
    st = pjmedia_aud_register_factory(&pjmedia_mpsx_audio_factory);
    printf("pj_phone: register mpsx aud factory -> %d\r\n", (int)st);

    /* Use the real mpsx audio/mic device (QEMU mpsx sound card) instead of
     * the null device.  The mpsx factory was registered just above, so
     * look up its device id and hand it to pjsua. */
    {
        pjmedia_aud_dev_index mpsx_dev = PJMEDIA_AUD_INVALID_DEV;
        st = pjmedia_aud_dev_lookup("mpsx", "mpsx audio/mic", &mpsx_dev);
        if (st == PJ_SUCCESS && mpsx_dev != PJMEDIA_AUD_INVALID_DEV) {
            st = pjsua_set_snd_dev(mpsx_dev, mpsx_dev);
            printf("pj_phone: pjsua_set_snd_dev(mpsx dev=%d) -> %d\r\n",
                   (int)mpsx_dev, (int)st);
        } else {
            printf("pj_phone: mpsx snd dev lookup failed (%d), "
                   "falling back to null\r\n", (int)st);
            st = pjsua_set_null_snd_dev();
            printf("pj_phone: pjsua_set_null_snd_dev -> %d\r\n", (int)st);
        }
    }
#else
    st = pjsua_set_null_snd_dev();
    printf("pj_phone: pjsua_set_null_snd_dev -> %d\r\n", (int)st);
#endif

    /* UDP transport bound to the guest SIP port (hostfwd'd as itself). */
    pjsua_transport_config_default(&tcfg);
    tcfg.port = GUEST_SIP_PORT;
    st = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &tcfg, &tp);
    if (st != PJ_SUCCESS) {
        printf("pj_phone: transport_create failed (%d)\r\n", st);
        return -1;
    }
    printf("pj_phone: UDP transport up on :%d (id=%d)\r\n", GUEST_SIP_PORT,
           (int)tp);

    st = pjsua_start();
    if (st != PJ_SUCCESS) {
        printf("pj_phone: pjsua_start failed (%d)\r\n", st);
        return -1;
    }
    printf("pj_phone: pjsua_start OK\r\n");

    /* Local account (no REGISTER for now; dial directly.  Set reg_uri to
     * register to a host SIP server later). */
    pjsua_acc_config_default(&acc_cfg);
    acc_cfg.id = pj_str("sip:phone@10.0.2.15");
    /* Advertise 127.0.0.1 as the media address in SDP so the host sends
     * RTP to the slirp hostfwd port (udp::4000-:4000 -> guest) instead of
     * an unreachable 10.0.2.15.  Binding stays 0.0.0.0. */
    acc_cfg.rtp_cfg.public_addr = pj_str("127.0.0.1");
    st = pjsua_acc_add(&acc_cfg, PJ_FALSE, &g_acc);
    if (st != PJ_SUCCESS) {
        printf("pj_phone: acc_add failed (%d)\r\n", st);
        return -1;
    }
    printf("pj_phone: account added id=%d\r\n", g_acc);

    /* Dial the host UA. */
    st = pj_phone_dial();
    if (st != PJ_SUCCESS)
        return -1;
    g_last_idle_tick = xTaskGetTickCount();

    return 0;
}
