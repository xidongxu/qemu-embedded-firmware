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
#include <pj/os.h>
#include <pjmedia/audiodev.h>
#include <pjmedia-audiodev/audiodev.h>
#include <pjsip/sip_msg.h>
#include <pjsip/sip_uri.h>
#include <pjsip/sip_endpoint.h>
#include <pj/timer.h>
#include <pjsua-lib/pjsua.h>
#include "mpsx_dev.h"

#define HOST_GW        "10.0.2.2"   /* slirp gateway = host loopback alias */
/* FreeSWITCH is configured to bind 0.0.0.0 (sip-ip/rtp-ip), so it is also
 * reachable on host loopback => fixed 10.0.2.2 (no dependency on the host's
 * DHCP-assigned LAN IP). */
#define FS_HOST        "10.0.2.2"
#define HOST_SIP_PORT  5060         /* FreeSWITCH SIP UDP port */
#define GUEST_SIP_PORT 15062        /* guest SIP port (hostfwd'd as itself) */
#define REG_USER       "1000"       /* extension registered on FreeSWITCH */
#define REG_PASSWORD   "1234"       /* FreeSWITCH default_password */
/* Dial host/port: where to reach SIP peers (the extension's home domain on
 * FreeSWITCH = the HOST LAN IP, where the Android phone 1005 registers).
 * This is the one value tied to the host's DHCP-assigned address; it is a
 * compile-time default here and can be overridden at runtime with
 * pj_phone_set_dial_host() so a host IP change needs no rebuild. */
#define PJ_PHONE_DIAL_HOST   "192.168.23.6"
#define PJ_PHONE_DIAL_PORT   5060

/* Optional auto-dial for scripted testing (no UI): define PJ_PHONE_AUTO_DIAL=1
 * to place a call to PJ_PHONE_DEFAULT_NUMBER once registered.  Off by default
 * - the LVGL UI drives calls interactively. */
#ifndef PJ_PHONE_AUTO_DIAL
#   define PJ_PHONE_AUTO_DIAL 0
#endif
#ifndef PJ_PHONE_DEFAULT_NUMBER
#   define PJ_PHONE_DEFAULT_NUMBER "1005"
#endif

static pjsua_acc_id g_acc = PJSUA_INVALID_ID;
static pjsua_call_id g_call_id = PJSUA_INVALID_ID;          /* active call */
static pjsua_call_id g_incoming_call_id = PJSUA_INVALID_ID; /* pending incoming */

/* UI-facing state.  Written on the pjsua worker thread (callbacks), read from
 * the UI task; single-core FreeRTOS so plain int + small char-buffer state is
 * safe enough, and writes are guarded with a critical section. */
static pj_phone_reg_state_t  g_reg_state  = PJ_PHONE_REG_UNREGISTERED;
static pj_phone_call_state_t g_call_state = PJ_PHONE_CALL_IDLE;
static char   g_peer[64] = "";            /* remote user (incoming/outgoing) */
static unsigned g_reg_attempts = 0;       /* failed REGISTER count (log only) */
static pj_time_val g_call_start;          /* CONFIRMED wall-clock, for duration */
#if PJ_PHONE_AUTO_DIAL
static int g_auto_dialed = 0;             /* PJ_PHONE_AUTO_DIAL fired once */
#endif

/* Runtime-configurable dial target (default from PJ_PHONE_DIAL_HOST). */
static char     g_dial_host[64] = PJ_PHONE_DIAL_HOST;
static unsigned g_dial_port    = PJ_PHONE_DIAL_PORT;

/* UI notification hook (called from the pjsua worker thread). */
static pj_phone_cb_t g_cb = NULL;
static void *g_cb_user = NULL;

static void phone_notify(void)
{
    if (g_cb)
        g_cb(g_cb_user);
}

/* ---- deferred call-control on the pjsua worker thread -----------
 * This port's pjsua is only reliably driven from its own worker thread:
 * calling pjsua_call_make_call() directly from the LVGL task spins inside
 * the media-transport setup (the async transport-ready never advances when
 * the caller is not the worker, so the UI freezes).  Call-control is posted
 * here as a one-shot pjsip timer whose callback runs on the worker thread -
 * the exact same context as the previously-working on_reg_state auto-dial. */
typedef enum {
    PHONE_JOB_NONE = 0,
    PHONE_JOB_DIAL,
    PHONE_JOB_ANSWER,
    PHONE_JOB_REJECT,
    PHONE_JOB_HANGUP
} phone_job_type_t;

static volatile phone_job_type_t g_job = PHONE_JOB_NONE;
static char g_job_number[64] = "";
static pj_timer_entry g_job_timer;

static pj_status_t phone_job_post(phone_job_type_t job, const char *number);

/* Runs on the pjsua worker thread (pjsip timer callback). */
static void phone_job_exec(pj_timer_heap_t *th, pj_timer_entry *entry)
{
    phone_job_type_t job;
    pjsua_call_id cid;

    PJ_UNUSED_ARG(th); PJ_UNUSED_ARG(entry);

    job = g_job;
    g_job = PHONE_JOB_NONE;

    switch (job) {
    case PHONE_JOB_DIAL: {
        pj_str_t uri;
        pjsua_call_id call_id;
        pj_status_t st;
        char buf[160];

        /* Build "sip:<ext>@<dial_host>:<port>" (or keep a full sip: URI). */
        if (pj_ansi_strnicmp(g_job_number, "sip:", 4) == 0) {
            snprintf(buf, sizeof(buf), "%s", g_job_number);
        } else {
            snprintf(buf, sizeof(buf), "sip:%s@%s:%u",
                     g_job_number, g_dial_host, g_dial_port);
        }
        pj_strset2(&uri, buf);
        st = pjsua_call_make_call(g_acc, &uri, NULL, NULL, NULL, &call_id);
        {
            pj_time_val t;
            pj_gettimeofday(&t);
            printf("pj_phone: [%lu.%03lu] make_call(%s) -> %d (call=%d)\r\n",
                   (unsigned long)t.sec, (unsigned long)t.msec,
                   buf, (int)st, (int)call_id);
        }
        if (st == PJ_SUCCESS) {
            taskENTER_CRITICAL();
            g_call_id = call_id;
            g_call_state = PJ_PHONE_CALL_DIALING;
            taskEXIT_CRITICAL();
            phone_notify();
        }
        break;
    }
    case PHONE_JOB_ANSWER:
        cid = g_incoming_call_id;
        if (cid != PJSUA_INVALID_ID) {
            printf("pj_phone: answer call %d (200)\r\n", (int)cid);
            pjsua_call_answer(cid, 200, NULL, NULL);
        }
        break;
    case PHONE_JOB_REJECT:
        cid = g_incoming_call_id;
        if (cid != PJSUA_INVALID_ID) {
            printf("pj_phone: reject call %d (486)\r\n", (int)cid);
            pjsua_call_answer(cid, 486, NULL, NULL);
        }
        break;
    case PHONE_JOB_HANGUP:
        cid = g_call_id;
        if (cid != PJSUA_INVALID_ID) {
            printf("pj_phone: hangup call %d\r\n", (int)cid);
            pjsua_call_hangup(cid, 0, NULL, NULL);
        }
        break;
    default:
        break;
    }
}

static pj_status_t phone_job_post(phone_job_type_t job, const char *number)
{
    pjsip_endpoint *endpt;
    pj_time_val delay;

    if (g_job != PHONE_JOB_NONE)
        return PJ_EBUSY;

    g_job = job;
    if (number) {
        strncpy(g_job_number, number, sizeof(g_job_number) - 1);
        g_job_number[sizeof(g_job_number) - 1] = '\0';
    }

    endpt = pjsua_get_pjsip_endpt();
    if (!endpt) {
        g_job = PHONE_JOB_NONE;
        return PJ_EINVALIDOP;
    }

    delay.sec = 0;
    delay.msec = 1;   /* fire on the very next worker event-loop pass */
    pj_timer_entry_init(&g_job_timer, 0, NULL, phone_job_exec);
    return pjsip_endpt_schedule_timer(endpt, &g_job_timer, &delay);
}

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
    /* Register this FreeRTOS task with PJLIB so the pjsua stream-stat calls
     * below don't hit the NULL-thread PJSUA_LOCK spin. */
    pj_thread_desc desc;
    memset(desc, 0, sizeof(desc));
    pj_thread_register("wd", desc, NULL);

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

    if (pjsua_acc_get_info(acc_id, &info) != PJ_SUCCESS)
        return;

    printf("pj_phone: acc %d reg state=%d (%.*s)\r\n", acc_id,
           (int)info.status, (int)info.status_text.slen,
           info.status_text.ptr);

    if (info.status >= 200 && info.status < 300) {
        /* Registered OK (2xx).  No auto-dial by default - the UI drives
         * calls via pj_phone_dial()/answer()/hangup().  Scripted testing
         * can enable PJ_PHONE_AUTO_DIAL to call a fixed number instead. */
        g_reg_state = PJ_PHONE_REG_REGISTERED;
#if PJ_PHONE_AUTO_DIAL
        if (!g_auto_dialed) {
            if (pj_phone_dial(PJ_PHONE_DEFAULT_NUMBER) == PJ_SUCCESS)
                g_auto_dialed = 1;
        }
#endif
    } else if (info.status != 0) {
        /* Registration failed (403/408/503/...).  pjsua retries the REGISTER
         * automatically every reg_first_retry_interval / reg_retry_interval
         * seconds (set in pj_phone_init); we just count + log. */
        printf("pj_phone: reg attempt %u FAILED (%d) - pjsua will retry\r\n",
               ++g_reg_attempts, (int)info.status);
        g_reg_state = PJ_PHONE_REG_FAILED;
    } else {
        /* info.status == 0 => REGISTER still in flight. */
        g_reg_state = PJ_PHONE_REG_REGISTERING;
    }
    phone_notify();
}

static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
                             pjsip_rx_data *rdata)
{
    char tmp[64] = "";
    PJ_UNUSED_ARG(acc_id);

    if (g_call_id != PJSUA_INVALID_ID && g_call_state != PJ_PHONE_CALL_IDLE) {
        /* already in a call -> busy */
        printf("pj_phone: busy, rejecting incoming call %d (486)\r\n", call_id);
        pjsua_call_answer(call_id, 486, NULL, NULL);
        return;
    }

    /* Remember the caller so the UI can show "Incoming from <user>". */
    if (rdata && rdata->msg_info.from) {
        pjsip_from_hdr *from = rdata->msg_info.from;
        if (from->uri && PJSIP_URI_SCHEME_IS_SIP(from->uri)) {
            pjsip_sip_uri *u = (pjsip_sip_uri *)from->uri;
            unsigned n = u->user.slen;
            if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
            memcpy(tmp, u->user.ptr, n);
            tmp[n] = '\0';
        }
    }

    printf("pj_phone: incoming call %d from '%s' - waiting for answer\r\n",
           call_id, tmp);

    taskENTER_CRITICAL();
    g_incoming_call_id = call_id;
    g_call_id = call_id;                 /* hangup can also end it */
    g_call_state = PJ_PHONE_CALL_INCOMING;
    strncpy(g_peer, tmp, sizeof(g_peer) - 1);
    g_peer[sizeof(g_peer) - 1] = '\0';
    taskEXIT_CRITICAL();
    phone_notify();
}

static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    pjsua_call_info ci;
    PJ_UNUSED_ARG(e);
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
        return;

    {
        pj_time_val t;
        pj_gettimeofday(&t);
        printf("pj_phone: [%lu.%03lu] call %d state=%d (%.*s)\r\n",
               (unsigned long)t.sec, (unsigned long)t.msec, call_id,
               (int)ci.state, (int)ci.state_text.slen, ci.state_text.ptr);
    }

    switch (ci.state) {
    case PJSIP_INV_STATE_CALLING:
    case PJSIP_INV_STATE_EARLY:
    case PJSIP_INV_STATE_CONNECTING:
        g_call_state = PJ_PHONE_CALL_DIALING;
        phone_notify();
        break;

    case PJSIP_INV_STATE_CONFIRMED:
        /* Remember when the call became active (duration log + UI timer). */
        if (call_id == g_call_id) {
            pj_gettimeofday(&g_call_start);
            g_call_state = PJ_PHONE_CALL_ACTIVE;
        }
        phone_notify();
        break;

    case PJSIP_INV_STATE_DISCONNECTED:
        /* Log why the call ended (remote hang-up / 486 / timeout / ...) plus
         * the call duration when it was established. */
        if (g_call_start.sec != 0) {
            pj_time_val now, dur;
            pj_gettimeofday(&now);
            dur = now;
            PJ_TIME_VAL_SUB(dur, g_call_start);
            printf("pj_phone: call %d disconnected: reason=%d (%.*s) "
                   "dur=%ldms\r\n",
                   call_id, (int)ci.last_status,
                   (int)ci.last_status_text.slen,
                   ci.last_status_text.ptr,
                   (long)PJ_TIME_VAL_MSEC(dur));
        } else {
            printf("pj_phone: call %d disconnected: reason=%d (%.*s) "
                   "(call never established)\r\n",
                   call_id, (int)ci.last_status,
                   (int)ci.last_status_text.slen,
                   ci.last_status_text.ptr);
        }
        if (call_id == g_call_id)
            g_call_id = PJSUA_INVALID_ID;
        if (call_id == g_incoming_call_id)
            g_incoming_call_id = PJSUA_INVALID_ID;
        g_call_state = PJ_PHONE_CALL_IDLE;
        g_call_start.sec = 0;
        g_call_start.msec = 0;
        phone_notify();
        break;

    default:
        break;
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
            printf("pj_phone: conf connected (call slot %d <-> snd 0)\r\n",
                   ci.conf_slot);
        }
    }
}

/* --------------------------- call control ------------------------ */
int pj_phone_dial(const char *number)
{
    if (!number || !*number || g_acc == PJSUA_INVALID_ID)
        return -1;

    /* Show the dial state immediately; the actual make_call runs on the
     * pjsua worker thread (see phone_job_exec) so the UI never blocks. */
    taskENTER_CRITICAL();
    g_call_state = PJ_PHONE_CALL_DIALING;
    strncpy(g_peer, number, sizeof(g_peer) - 1);
    g_peer[sizeof(g_peer) - 1] = '\0';
    taskEXIT_CRITICAL();
    phone_notify();

    return (int)phone_job_post(PHONE_JOB_DIAL, number);
}

/* --------------------------- entry ------------------------------- */
int pj_phone_init(void)
{
    pjsua_config cfg;
    pjsua_logging_config log_cfg;
    pjsua_media_config media_cfg;
    pjsua_transport_config tcfg;
    pjsua_acc_config acc_cfg;
    pjsua_transport_id tp;
    pj_status_t st;
    char id_buf[128];   /* acc_cfg.id must stay valid until pjsua_acc_add() */

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

    /* UDP transport bound to the guest SIP port (hostfwd'd as itself).
     * Advertise 127.0.0.1 as the transport's public address so the SIP
     * Contact/Via (hence inbound INVITE/BYE from the host FreeSWITCH) is
     * reachable through the slirp hostfwd udp::15062 -> guest:15062.  Without
     * this the guest's Contact is sip:1000@10.0.2.15:15062, which the host
     * cannot route to -> incoming calls fail ("not in service") and remote
     * hang-ups (BYE) are never delivered.  Same pattern as rtp_cfg.public_addr
     * below. */
    pjsua_transport_config_default(&tcfg);
    tcfg.port = GUEST_SIP_PORT;
    tcfg.public_addr = pj_str("127.0.0.1");
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

    /* Register extension 1000 in the SAME domain as the phone (the internal/
     * default profile = the host LAN IP = g_dial_host).  The phone's dialplan
     * looks up the callee in the CALLER's domain (sofia_contact(1000@<lan-ip>));
     * if the guest registered only in the loopback domain (10.0.2.2), the
     * phone gets "not in service" and the remote BYE is never delivered.
     * The REGISTER still goes to the loopback leg (reg_uri = 10.0.2.2) so the
     * Contact (127.0.0.1:15062, transport public_addr) is NOT NAT-rewritten
     * by the internal profile (nat.auto does not cover loopback), while the
     * To domain (id) is the LAN IP so FreeSWITCH binds 1000 under the LAN
     * domain.  g_dial_host is runtime-configurable via pj_phone_set_dial_host(),
     * so a host IP change needs no rebuild.  realm "*" matches FS's realm. */
    pjsua_acc_config_default(&acc_cfg);
    snprintf(id_buf, sizeof(id_buf), "sip:%s@%s", REG_USER, g_dial_host);
    acc_cfg.id = pj_str(id_buf);
    acc_cfg.reg_uri = pj_str("sip:" FS_HOST ":5060");
    /* Outbound proxy: route ALL outbound requests (REGISTER + INVITE) via the
     * loopback leg (10.0.2.2 = internal-lo profile).  This keeps the outgoing
     * call's dialog on the SAME profile as the registration, so FreeSWITCH
     * sends the remote-hangup BYE back to the guest's Contact
     * (127.0.0.1:15062, reachable via hostfwd) instead of to the AOR
     * (192.168.23.6, unreachable) - which is why an Android hang-up on an
     * OUTGOING call was never detected (FS log: "BYE to sofia/internal/
     * 1000@192.168.23.6" without a port).  The Request-URI keeps the LAN
     * domain (1005@192.168.23.6) so routing to the phone is unaffected, and
     * the source stays 127.0.0.1 (out of FS's nat.auto ACL) so the Contact is
     * never NAT-rewritten. */
    acc_cfg.proxy[acc_cfg.proxy_cnt++] = pj_str("sip:" FS_HOST ":5060");
    acc_cfg.cred_count = 1;
    acc_cfg.cred_info[0].realm = pj_str("*");
    acc_cfg.cred_info[0].scheme = pj_str("digest");
    acc_cfg.cred_info[0].username = pj_str(REG_USER);
    acc_cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    acc_cfg.cred_info[0].data = pj_str(REG_PASSWORD);
    /* Advertise 127.0.0.1 as the media address in SDP so the host sends
     * RTP to the slirp hostfwd port (udp::4000-:4000 -> guest) instead of
     * an unreachable 10.0.2.15.  Binding stays 0.0.0.0. */
    acc_cfg.rtp_cfg.public_addr = pj_str("127.0.0.1");
    /* Retry failed REGISTERs every 5s instead of the pjsua defaults
     * (60s first / 300s later) so a transient failure recovers fast.
     * on_reg_state only dials after a 2xx comes back. */
    acc_cfg.reg_first_retry_interval = 5;
    acc_cfg.reg_retry_interval = 5;
    st = pjsua_acc_add(&acc_cfg, PJ_TRUE, &g_acc);
    if (st != PJ_SUCCESS) {
        printf("pj_phone: acc_add failed (%d)\r\n", st);
        return -1;
    }
    printf("pj_phone: account added id=%d\r\n", g_acc);

    /* Registered account is up (registration is async; the UI shows the
     * status).  No auto-dial - the UI drives calls. */
    return 0;
}

/* --------------------------- public API -------------------------- */
void pj_phone_set_callback(pj_phone_cb_t cb, void *user_data)
{
    g_cb = cb;
    g_cb_user = user_data;
}

void pj_phone_set_dial_host(const char *host, unsigned port)
{
    taskENTER_CRITICAL();
    if (host && *host) {
        strncpy(g_dial_host, host, sizeof(g_dial_host) - 1);
        g_dial_host[sizeof(g_dial_host) - 1] = '\0';
    }
    g_dial_port = port;
    taskEXIT_CRITICAL();
    printf("pj_phone: dial host set to %s:%u\r\n", g_dial_host, g_dial_port);
}

int pj_phone_answer(void)
{
    return (int)phone_job_post(PHONE_JOB_ANSWER, NULL);
}

int pj_phone_reject(void)
{
    return (int)phone_job_post(PHONE_JOB_REJECT, NULL);
}

int pj_phone_hangup(void)
{
    return (int)phone_job_post(PHONE_JOB_HANGUP, NULL);
}

pj_phone_reg_state_t pj_phone_get_reg_state(void)
{
    return g_reg_state;
}

pj_phone_call_state_t pj_phone_get_call_state(void)
{
    return g_call_state;
}

const char *pj_phone_get_peer_number(void)
{
    return g_peer;
}

unsigned long pj_phone_get_call_duration_ms(void)
{
    pj_time_val now, dur;

    if (g_call_state != PJ_PHONE_CALL_ACTIVE || g_call_start.sec == 0)
        return 0;
    pj_gettimeofday(&now);
    dur = now;
    PJ_TIME_VAL_SUB(dur, g_call_start);
    return (unsigned long)PJ_TIME_VAL_MSEC(dur);
}
