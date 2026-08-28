/*
 * pj_phone.c - PJSUA high-level phone application (experiment).
 *
 * Uses the pjsua high-level API (pjsua_create/init/transport/acc/call) to
 * place SIP calls through QEMU slirp + hostfwd.
 *
 * Topology (slirp user-net + hostfwd):
 *   guest SIP  : bind 0.0.0.0:<GUEST_SIP_PORT>  (hostfwd udp::<port>-:<port>)
 *   dial target: sip:user@10.0.2.2:<HOST_SIP_PORT>  (gateway = host loopback)
 *   host responds to the Via "received" (127.0.0.1:<port>) -> hostfwd -> guest
 *
 * Audio: mpsx audio/mic (QEMU virtual sound card) via pjmedia-audiodev.
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
#include <pj/pool.h>
#include <pjmedia/audiodev.h>
#include <pjmedia-audiodev/audiodev.h>
#include <pjsip/sip_msg.h>
#include <pjsip/sip_uri.h>
#include <pjsip/sip_endpoint.h>
#include <pj/timer.h>
#include <pjsua-lib/pjsua.h>
#include "mpsx_dev.h"

/* Slirp gateway = host loopback alias (fixed, independent of host DHCP). */
#define HOST_GW "10.0.2.2"

/* FreeSWITCH binds 0.0.0.0 so it is reachable on host loopback. */
#define FS_HOST "10.0.2.2"
#define HOST_SIP_PORT 5060

/* Guest SIP port (hostfwd'd as itself). */
#define GUEST_SIP_PORT 15062

/* Extension registered on FreeSWITCH. */
#define REG_USER "1000"
#define REG_PASSWORD "1234"

/* Dial host/port: where to reach SIP peers (the extension's home domain on
 * FreeSWITCH = the HOST LAN IP, where the Android phone 1005 registers).
 * This is the one value tied to the host's DHCP-assigned address.  It can be
 * overridden at runtime with pj_phone_set_dial_host() + pj_phone_reregister()
 * so a host IP change needs no rebuild. */
#define PJ_PHONE_DIAL_HOST "192.168.23.7"
#define PJ_PHONE_DIAL_PORT 5060

/* Optional auto-dial for scripted testing (no UI). */
#ifndef PJ_PHONE_AUTO_DIAL
#define PJ_PHONE_AUTO_DIAL 0
#endif
#ifndef PJ_PHONE_DEFAULT_NUMBER
#define PJ_PHONE_DEFAULT_NUMBER "1005"
#endif

/* Auto-hangup once inbound media has stalled this long while in a call
 * (0 = never auto-hangup, only report). */
#ifndef PJ_PHONE_MEDIA_STALL_HANGUP_MS
#define PJ_PHONE_MEDIA_STALL_HANGUP_MS 60000
#endif

/* Active account and call ids. */
static pjsua_acc_id g_acc = PJSUA_INVALID_ID;
static pjsua_call_id g_call_id = PJSUA_INVALID_ID;
static pjsua_call_id g_incoming_call_id = PJSUA_INVALID_ID;

/* UI-facing state.  Written on the pjsua worker thread (callbacks), read from
 * the UI task. */
static pj_phone_reg_state_t g_reg_state = PJ_PHONE_REG_UNREGISTERED;
static pj_phone_call_state_t g_call_state = PJ_PHONE_CALL_IDLE;
static char g_peer[64] = "";
static unsigned g_reg_attempts = 0;
static pj_time_val g_call_start = {0, 0};

/* How the last call ended (SIP status code + text). */
static int g_last_call_status = 0;
static char g_last_call_status_text[64] = "";

/* Inbound-media stall detection (set by the watchdog). */
static volatile int g_media_stall = 0;
static pj_time_val g_media_stall_at = {0, 0};
static volatile int g_media_stall_hung = 0;

/* Most recently received DTMF digits (shift buffer, for the UI display). */
static char g_rx_dtmf[16] = "";

#if PJ_PHONE_AUTO_DIAL
static int g_auto_dialed = 0;
#endif

/* Runtime-configurable dial target. */
static char g_dial_host[64] = PJ_PHONE_DIAL_HOST;
static unsigned g_dial_port = PJ_PHONE_DIAL_PORT;

/* UI notification hook (called from the pjsua worker thread). */
static pj_phone_cb_t g_cb = NULL;
static void *g_cb_user = NULL;

static void phone_notify(void) {
    if (g_cb != NULL) {
        g_cb(g_cb_user);
    }
}

/* Roll back to IDLE (clears both call ids and the call state). */
static void phone_to_idle(void) {
    taskENTER_CRITICAL();
    g_call_id = PJSUA_INVALID_ID;
    g_incoming_call_id = PJSUA_INVALID_ID;
    g_call_state = PJ_PHONE_CALL_IDLE;
    taskEXIT_CRITICAL();
    phone_notify();
}

/* Deferred call-control on the pjsua worker thread.
 *
 * This port's pjsua is only reliably driven from its own worker thread:
 * calling pjsua_call_make_call() directly from the LVGL task spins inside
 * the media-transport setup (the async transport-ready never advances when
 * the caller is not the worker, so the UI freezes).  Call-control is posted
 * here as a one-shot pjsip timer whose callback runs on the worker thread. */
typedef enum {
    PHONE_JOB_NONE = 0,
    PHONE_JOB_DIAL,
    PHONE_JOB_ANSWER,
    PHONE_JOB_REJECT,
    PHONE_JOB_HANGUP,
    PHONE_JOB_REREG
} phone_job_type_t;

static volatile phone_job_type_t g_job = PHONE_JOB_NONE;
static char g_job_number[64] = "";
static pj_timer_entry g_job_timer;

static pj_status_t phone_job_post(phone_job_type_t job, const char *number);

/* Runs on the pjsua worker thread (pjsip timer callback). */
static void phone_job_exec(pj_timer_heap_t *th, pj_timer_entry *entry) {
    phone_job_type_t job = PHONE_JOB_NONE;
    pjsua_call_id cid = PJSUA_INVALID_ID;

    PJ_UNUSED_ARG(th);
    PJ_UNUSED_ARG(entry);

    job = g_job;
    g_job = PHONE_JOB_NONE;

    switch (job) {
    case PHONE_JOB_DIAL: {
        pj_str_t uri;
        pjsua_call_id call_id = PJSUA_INVALID_ID;
        pj_status_t st = PJ_SUCCESS;
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
            pj_time_val t = {0, 0};
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
        } else {
            /* make_call failed: roll back to IDLE so the UI does not sit in
             * DIALING forever. */
            printf("pj_phone: make_call(%s) FAILED (%d) - back to IDLE\r\n",
                   buf, (int)st);
            phone_to_idle();
        }
        break;
    }
    case PHONE_JOB_ANSWER:
        cid = g_incoming_call_id;
        if (cid != PJSUA_INVALID_ID) {
            printf("pj_phone: answer call %d (200)\r\n", (int)cid);
            if (pjsua_call_answer(cid, 200, NULL, NULL) != PJ_SUCCESS) {
                /* Answer failed: the call is likely gone. */
                printf("pj_phone: answer FAILED - back to IDLE\r\n");
                phone_to_idle();
            }
        }
        break;
    case PHONE_JOB_REJECT:
        cid = g_incoming_call_id;
        if (cid != PJSUA_INVALID_ID) {
            printf("pj_phone: reject call %d (486)\r\n", (int)cid);
            if (pjsua_call_answer(cid, 486, NULL, NULL) != PJ_SUCCESS) {
                printf("pj_phone: reject FAILED - back to IDLE\r\n");
                phone_to_idle();
            }
        }
        break;
    case PHONE_JOB_HANGUP:
        cid = g_call_id;
        if (cid != PJSUA_INVALID_ID) {
            printf("pj_phone: hangup call %d\r\n", (int)cid);
            if (pjsua_call_hangup(cid, 0, NULL, NULL) != PJ_SUCCESS) {
                /* Hanging up an already-gone call is harmless; the
                 * DISCONNECTED callback cleans up the state. */
                printf("pj_phone: hangup returned error (ignored)\r\n");
            }
        }
        break;
    case PHONE_JOB_REREG: {
        /* Re-register under the CURRENT g_dial_host.  Used after the host's
         * DHCP IP changes so the 403 "can't find user@old-IP" can be fixed
         * at runtime without a rebuild. */
        pjsua_acc_config ac;
        pj_pool_t *pool = NULL;
        char id_buf[128];
        pj_status_t st = PJ_SUCCESS;

        if (g_acc == PJSUA_INVALID_ID) {
            printf("pj_phone: re-register skipped (no account)\r\n");
            break;
        }
        pool = pjsua_pool_create("rereg", 512, 256);
        if (pool == NULL) {
            printf("pj_phone: re-register skipped (pool alloc failed)\r\n");
            break;
        }
        pjsua_acc_get_config(g_acc, pool, &ac);
        snprintf(id_buf, sizeof(id_buf), "sip:%s@%s", REG_USER, g_dial_host);
        ac.id = pj_str(id_buf);
        st = pjsua_acc_modify(g_acc, &ac);
        pj_pool_release(pool);
        printf("pj_phone: re-register as %s -> %d\r\n", id_buf, (int)st);
        if (st == PJ_SUCCESS) {
            g_reg_state = PJ_PHONE_REG_REGISTERING;
            phone_notify();
            pjsua_acc_set_registration(g_acc, PJ_TRUE);
        }
        break;
    }
    default:
        break;
    }
}

static pj_status_t phone_job_post(phone_job_type_t job, const char *number) {
    pjsip_endpoint *endpt = NULL;
    pj_time_val delay = {0, 0};
    pj_status_t st = PJ_SUCCESS;

    if (g_job != PHONE_JOB_NONE) {
        return PJ_EBUSY;
    }

    g_job = job;
    if (number != NULL) {
        strncpy(g_job_number, number, sizeof(g_job_number) - 1);
        g_job_number[sizeof(g_job_number) - 1] = '\0';
    }

    endpt = pjsua_get_pjsip_endpt();
    if (endpt == NULL) {
        g_job = PHONE_JOB_NONE;
        return PJ_EINVALIDOP;
    }

    delay.sec = 0;
    delay.msec = 1;
    pj_timer_entry_init(&g_job_timer, 0, NULL, phone_job_exec);
    st = pjsip_endpt_schedule_timer(endpt, &g_job_timer, &delay);
    if (st != PJ_SUCCESS) {
        /* If the timer could not be scheduled the job would never run and
         * g_job would stay non-NONE, wedging every later call-control call. */
        g_job = PHONE_JOB_NONE;
    }
    return st;
}

/* Task state name for the watchdog report. */
static const char *wd_state_name(int s) {
    switch (s) {
    case 0: return "RUN";
    case 1: return "RDY";
    case 2: return "BLK";
    case 3: return "SUS";
    default: return "?";
    }
}

/* High-priority watchdog: reports heap / stack high-water / RTP stats even if
 * the pjsua worker thread stalls. */
void phone_watchdog(void *arg) {
    pj_thread_desc desc;
    TaskHandle_t cur = NULL;
    UBaseType_t n = 0;
    TaskStatus_t *st = NULL;
    UBaseType_t i = 0;
    static unsigned long wd_last_rx = 0;
    static int wd_stall_cnt = 0;

    /* Register this FreeRTOS task with PJLIB so the pjsua stream-stat calls
     * below don't hit the NULL-thread PJSUA_LOCK spin. */
    memset(desc, 0, sizeof(desc));
    pj_thread_register("wd", desc, NULL);

    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));

        /* RTP stream stat for audio media (index 0). */
        if (g_call_id != PJSUA_INVALID_ID) {
            pjsua_stream_stat ss;
            int stalled = 0;

            if (pjsua_call_get_stream_stat(g_call_id, 0, &ss) == PJ_SUCCESS) {
                /* Media-stall monitor: RTP is continuous (no_vad), so an
                 * inbound counter that is not advancing while we are still
                 * transmitting means the inbound media path has died.  Mark a
                 * stall (UI shows "No audio!") and auto-hangup after
                 * PJ_PHONE_MEDIA_STALL_HANGUP_MS. */
                stalled = (ss.rtcp.tx.pkt != 0 && ss.rtcp.rx.pkt == wd_last_rx);
                printf("wd: rx_pkt=%lu tx_pkt=%lu rx_lost=%lu%s\r\n",
                       (unsigned long)ss.rtcp.rx.pkt,
                       (unsigned long)ss.rtcp.tx.pkt,
                       (unsigned long)ss.rtcp.rx.loss,
                       stalled ? " [RX-STALL]" : "");
                if (stalled) {
                    pj_time_val t = {0, 0};
                    pj_gettimeofday(&t);
                    if (++wd_stall_cnt >= 5) {
                        wd_stall_cnt = 0;
                        if (!g_media_stall) {
                            g_media_stall = 1;
                            g_media_stall_at = t;
                            printf("wd: media RX stalled - marking stall "
                                   "(auto-hangup in %lu ms)\r\n",
                                   (unsigned long)PJ_PHONE_MEDIA_STALL_HANGUP_MS);
                        } else if (PJ_PHONE_MEDIA_STALL_HANGUP_MS > 0 &&
                                   !g_media_stall_hung) {
                            pj_time_val el = t;
                            PJ_TIME_VAL_SUB(el, g_media_stall_at);
                            if (PJ_TIME_VAL_MSEC(el) >=
                                (long)PJ_PHONE_MEDIA_STALL_HANGUP_MS) {
                                printf("wd: media RX stalled too long - "
                                       "auto-hangup\r\n");
                                g_media_stall_hung = 1;
                                phone_job_post(PHONE_JOB_HANGUP, NULL);
                            }
                        }
                    }
                } else {
                    if (g_media_stall) {
                        g_media_stall = 0;
                        g_media_stall_hung = 0;
                        printf("wd: media RX recovered\r\n");
                    }
                    wd_stall_cnt = 0;
                }
                wd_last_rx = ss.rtcp.rx.pkt;
            }

            /* Conference signal level of the call slot. */
            {
                pjsua_call_info ci;
                unsigned tx = 0;
                unsigned rx = 0;
                if (pjsua_call_get_info(g_call_id, &ci) == PJ_SUCCESS &&
                    ci.conf_slot != PJSUA_INVALID_ID) {
                    pjsua_conf_get_signal_level(ci.conf_slot, &tx, &rx);
                    printf("wd: conf sig tx=%u rx=%u\r\n", tx, rx);
                }
            }
        }

        cur = xTaskGetCurrentTaskHandle();
        printf("wd: current=%s\r\n", pcTaskGetName(cur));
        n = uxTaskGetNumberOfTasks();
        st = pvPortMalloc(n * sizeof(TaskStatus_t));
        if (st != NULL) {
            n = uxTaskGetSystemState(st, n, NULL);
            for (i = 0; i < n; i++) {
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

/* Registration state callback. */
static void on_reg_state(pjsua_acc_id acc_id) {
    pjsua_acc_info info;

    if (pjsua_acc_get_info(acc_id, &info) != PJ_SUCCESS) {
        return;
    }

    printf("pj_phone: acc %d reg state=%d (%.*s)\r\n", acc_id,
           (int)info.status, (int)info.status_text.slen,
           info.status_text.ptr);

    if (info.status >= 200 && info.status < 300) {
        /* Registered OK (2xx).  The UI drives calls; no auto-dial by default. */
        g_reg_state = PJ_PHONE_REG_REGISTERED;
#if PJ_PHONE_AUTO_DIAL
        if (!g_auto_dialed) {
            if (pj_phone_dial(PJ_PHONE_DEFAULT_NUMBER) == PJ_SUCCESS) {
                g_auto_dialed = 1;
            }
        }
#endif
    } else if (info.status != 0) {
        /* Registration failed (403/408/503/...).  pjsua retries automatically
         * every reg_retry_interval seconds; we just count and log. */
        printf("pj_phone: reg attempt %u FAILED (%d) - pjsua will retry\r\n",
               ++g_reg_attempts, (int)info.status);
        g_reg_state = PJ_PHONE_REG_FAILED;
    } else {
        /* info.status == 0: REGISTER still in flight. */
        g_reg_state = PJ_PHONE_REG_REGISTERING;
    }
    phone_notify();
}

/* Incoming call callback. */
static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
                             pjsip_rx_data *rdata) {
    char tmp[64] = "";
    PJ_UNUSED_ARG(acc_id);

    if (g_call_id != PJSUA_INVALID_ID && g_call_state != PJ_PHONE_CALL_IDLE) {
        /* Already in a call -> busy. */
        printf("pj_phone: busy, rejecting incoming call %d (486)\r\n", call_id);
        if (pjsua_call_answer(call_id, 486, NULL, NULL) != PJ_SUCCESS) {
            printf("pj_phone: busy-reject FAILED for call %d\r\n", (int)call_id);
        }
        return;
    }

    /* Remember the caller so the UI can show "Incoming from <user>". */
    if (rdata != NULL && rdata->msg_info.from != NULL) {
        pjsip_from_hdr *from = rdata->msg_info.from;
        if (from->uri != NULL && PJSIP_URI_SCHEME_IS_SIP(from->uri)) {
            pjsip_sip_uri *u = (pjsip_sip_uri *)from->uri;
            unsigned n = u->user.slen;
            if (n >= sizeof(tmp)) {
                n = sizeof(tmp) - 1;
            }
            memcpy(tmp, u->user.ptr, n);
            tmp[n] = '\0';
        }
    }

    printf("pj_phone: incoming call %d from '%s' - waiting for answer\r\n",
           call_id, tmp);

    taskENTER_CRITICAL();
    g_incoming_call_id = call_id;
    g_call_id = call_id;
    g_call_state = PJ_PHONE_CALL_INCOMING;
    strncpy(g_peer, tmp, sizeof(g_peer) - 1);
    g_peer[sizeof(g_peer) - 1] = '\0';
    taskEXIT_CRITICAL();
    phone_notify();
}

/* Call state callback. */
static void on_call_state(pjsua_call_id call_id, pjsip_event *e) {
    pjsua_call_info ci;
    pj_time_val t = {0, 0};

    PJ_UNUSED_ARG(e);
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS) {
        return;
    }

    pj_gettimeofday(&t);
    printf("pj_phone: [%lu.%03lu] call %d state=%d (%.*s)\r\n",
           (unsigned long)t.sec, (unsigned long)t.msec, call_id,
           (int)ci.state, (int)ci.state_text.slen, ci.state_text.ptr);

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
        /* Log why the call ended plus the duration when it was established. */
        if (g_call_start.sec != 0) {
            pj_time_val now = {0, 0};
            pj_time_val dur = {0, 0};
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

        /* Record why the call ended so the UI can show it. */
        g_last_call_status = (int)ci.last_status;
        snprintf(g_last_call_status_text, sizeof(g_last_call_status_text),
                 "%.*s", (int)ci.last_status_text.slen,
                 ci.last_status_text.ptr);

        if (call_id == g_call_id) {
            g_call_id = PJSUA_INVALID_ID;
        }
        if (call_id == g_incoming_call_id) {
            g_incoming_call_id = PJSUA_INVALID_ID;
        }
        g_call_state = PJ_PHONE_CALL_IDLE;
        g_call_start.sec = 0;
        g_call_start.msec = 0;
        g_media_stall = 0;
        g_media_stall_hung = 0;
        g_rx_dtmf[0] = '\0';
        phone_notify();
        break;

    default:
        break;
    }
}

/* Call media state callback. */
static void on_call_media_state(pjsua_call_id call_id) {
    pjsua_call_info ci;

    if (pjsua_call_get_info(call_id, &ci) == PJ_SUCCESS) {
        printf("pj_phone: call %d media_status=%d%s\r\n", call_id,
               (int)ci.media_status,
               ci.media_status == PJSUA_CALL_MEDIA_ACTIVE ? " (ACTIVE)" : "");

        /* Wire the call's conference slot to the sound device (slot 0) once
         * media is up, so the real mpsx audio/mic is used. */
        if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) {
            /* pjsua does NOT create the echo canceller automatically; it must
             * be enabled explicitly once the sound device exists. */
            {
                pj_status_t e = pjsua_set_ec(200, 0);
                printf("pj_phone: pjsua_set_ec(tail=200) -> %d\r\n", (int)e);
                if (e != PJ_SUCCESS) {
                    printf("pj_phone: pjsua_set_ec FAILED (%d) - "
                           "continuing without EC\r\n", (int)e);
                }
            }
            if (pjsua_conf_connect(ci.conf_slot, 0) != PJ_SUCCESS) {
                printf("pj_phone: conf_connect(call->snd) FAILED\r\n");
            }
            if (pjsua_conf_connect(0, ci.conf_slot) != PJ_SUCCESS) {
                printf("pj_phone: conf_connect(snd->call) FAILED\r\n");
            }
            printf("pj_phone: conf connected (call slot %d <-> snd 0)\r\n",
                   ci.conf_slot);
        }
    }
}

/* DTMF receive callback (RFC 2833 / SIP INFO).  Runs on the worker thread. */
static void on_dtmf_digit(pjsua_call_id call_id, int digit) {
    char ch = (char)digit;
    size_t len = strlen(g_rx_dtmf);

    if (ch == 0) {
        return;
    }
    /* Shift the buffer to keep the most recent digits. */
    if (len >= sizeof(g_rx_dtmf) - 1) {
        memmove(g_rx_dtmf, g_rx_dtmf + 1, sizeof(g_rx_dtmf) - 2);
        len = sizeof(g_rx_dtmf) - 2;
    }
    g_rx_dtmf[len] = ch;
    g_rx_dtmf[len + 1] = '\0';
    printf("pj_phone: call %d DTMF rx '%c' (buf=%s)\r\n",
           (int)call_id, ch, g_rx_dtmf);
    phone_notify();
}

/* Dial an extension. */
int pj_phone_dial(const char *number) {
    pj_status_t st = PJ_SUCCESS;

    if (number == NULL || !*number || g_acc == PJSUA_INVALID_ID) {
        return -1;
    }

    /* Show the dial state immediately; the actual make_call runs on the
     * pjsua worker thread (see phone_job_exec) so the UI never blocks. */
    taskENTER_CRITICAL();
    g_call_state = PJ_PHONE_CALL_DIALING;
    strncpy(g_peer, number, sizeof(g_peer) - 1);
    g_peer[sizeof(g_peer) - 1] = '\0';
    taskEXIT_CRITICAL();
    phone_notify();

    st = phone_job_post(PHONE_JOB_DIAL, number);
    if (st != PJ_SUCCESS) {
        /* The job could not even be queued: roll back the optimistic DIALING
         * so the UI doesn't stay stuck. */
        printf("pj_phone: dial post FAILED (%d) - back to IDLE\r\n", (int)st);
        phone_to_idle();
    }
    return (int)st;
}

/* Send DTMF digits during an active call (RFC 2833). */
int pj_phone_send_dtmf(const char *digits) {
    pjsua_call_id cid = PJSUA_INVALID_ID;
    pj_str_t str;
    pj_status_t st = PJ_SUCCESS;

    if (digits == NULL || !*digits || g_call_state != PJ_PHONE_CALL_ACTIVE) {
        printf("pj_phone: send_dtmf rejected (no active call)\r\n");
        return -1;
    }
    taskENTER_CRITICAL();
    cid = g_call_id;
    taskEXIT_CRITICAL();
    if (cid == PJSUA_INVALID_ID) {
        printf("pj_phone: send_dtmf rejected (bad call id)\r\n");
        return -1;
    }
    pj_strset2(&str, (char *)digits);
    st = pjsua_call_dial_dtmf(cid, &str);
    if (st != PJ_SUCCESS) {
        printf("pj_phone: dial_dtmf(\"%s\") FAILED (%d)\r\n", digits, (int)st);
    }
    return (int)st;
}

/* Get the most recently received DTMF digits. */
int pj_phone_get_rx_dtmf(char *buf, int size) {
    if (buf == NULL || size <= 0) {
        return 0;
    }
    buf[0] = '\0';
    strncpy(buf, g_rx_dtmf, (size_t)size - 1);
    buf[size - 1] = '\0';
    return (int)strlen(buf);
}

/* Initialise the phone. */
int pj_phone_init(void) {
    pjsua_config cfg;
    pjsua_logging_config log_cfg;
    pjsua_media_config media_cfg;
    pjsua_transport_config tcfg;
    pjsua_acc_config acc_cfg;
    pjsua_transport_id tp = PJSUA_INVALID_ID;
    pj_status_t st = PJ_SUCCESS;
    char id_buf[128];

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
    cfg.cb.on_dtmf_digit = &on_dtmf_digit;

    pjsua_logging_config_default(&log_cfg);
    /* Level 2 = warnings+errors only; higher levels flood the serial port
     * with per-frame media logs which dominates guest CPU under TCG. */
    log_cfg.console_level = 2;

    pjsua_media_config_default(&media_cfg);
    media_cfg.clock_rate = 8000;
    media_cfg.snd_clock_rate = 8000;
    media_cfg.channel_count = 1;
    media_cfg.ec_options = 0;
    /* Disable VAD so the guest keeps sending (silence) and the RTP path
     * stays active. */
    media_cfg.no_vad = PJ_TRUE;
    /* Disable the auto-close of the sound device (default 1s idle) which
     * would kill the media clock -> guest stops sending RTP. */
    media_cfg.snd_auto_close_time = -1;
    /* Use the mpsx sound device's native clock (the default software clock
     * desynchronises from the mpsx DONE interrupts -> silence captured). */
    media_cfg.snd_use_sw_clock = PJ_FALSE;

    st = pjsua_init(&cfg, &log_cfg, &media_cfg);
    if (st != PJ_SUCCESS) {
        printf("pj_phone: pjsua_init failed (%d)\r\n", st);
        pjsua_destroy();
        return -1;
    }
    printf("pj_phone: pjsua_init OK\r\n");

#if PJMEDIA_AUDIO_DEV_HAS_MPSX
    /* Register the mpsx audio factory at runtime using the public
     * pjmedia_aud_register_factory() API. */
    st = pjmedia_aud_register_factory(&pjmedia_mpsx_audio_factory);
    printf("pj_phone: register mpsx aud factory -> %d\r\n", (int)st);

    /* Use the real mpsx audio/mic device; fall back to null on failure. */
    {
        pjmedia_aud_dev_index mpsx_dev = PJMEDIA_AUD_INVALID_DEV;
        st = pjmedia_aud_dev_lookup("mpsx", "mpsx audio/mic", &mpsx_dev);
        if (st == PJ_SUCCESS && mpsx_dev != PJMEDIA_AUD_INVALID_DEV) {
            st = pjsua_set_snd_dev(mpsx_dev, mpsx_dev);
            printf("pj_phone: pjsua_set_snd_dev(mpsx dev=%d) -> %d\r\n",
                   (int)mpsx_dev, (int)st);
            if (st != PJ_SUCCESS) {
                /* Sound-device selection failed - fall back to the null
                 * device so signalling still works. */
                printf("pj_phone: set_snd_dev(mpsx) failed (%d) - "
                       "falling back to null\r\n", (int)st);
                st = pjsua_set_null_snd_dev();
                printf("pj_phone: pjsua_set_null_snd_dev -> %d\r\n", (int)st);
            }
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
     * reachable through the slirp hostfwd udp::15062 -> guest:15062. */
    pjsua_transport_config_default(&tcfg);
    tcfg.port = GUEST_SIP_PORT;
    tcfg.public_addr = pj_str("127.0.0.1");
    st = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &tcfg, &tp);
    if (st != PJ_SUCCESS) {
        printf("pj_phone: transport_create failed (%d)\r\n", st);
        pjsua_destroy();
        return -1;
    }
    printf("pj_phone: UDP transport up on :%d (id=%d)\r\n", GUEST_SIP_PORT,
           (int)tp);

    st = pjsua_start();
    if (st != PJ_SUCCESS) {
        printf("pj_phone: pjsua_start failed (%d)\r\n", st);
        pjsua_destroy();
        return -1;
    }
    printf("pj_phone: pjsua_start OK\r\n");

    /* Register extension 1000 in the SAME domain as the phone (the internal/
     * default profile = the host LAN IP = g_dial_host).  The REGISTER still
     * goes to the loopback leg (reg_uri = 10.0.2.2) so the Contact
     * (127.0.0.1:15062) is NOT NAT-rewritten by the internal profile, while
     * the To domain (id) is the LAN IP so FreeSWITCH binds 1000 under the
     * LAN domain.  realm "*" matches FS's realm. */
    pjsua_acc_config_default(&acc_cfg);
    snprintf(id_buf, sizeof(id_buf), "sip:%s@%s", REG_USER, g_dial_host);
    acc_cfg.id = pj_str(id_buf);
    acc_cfg.reg_uri = pj_str("sip:" FS_HOST ":5060");
    /* Outbound proxy: route ALL outbound requests (REGISTER + INVITE) via the
     * loopback leg (10.0.2.2 = internal-lo profile) so the outgoing call's
     * dialog is on the SAME profile as the registration.  This makes the
     * remote-hangup BYE return to the guest's Contact (127.0.0.1:15062)
     * instead of an unreachable AOR.  The Request-URI keeps the LAN domain
     * so routing to the phone is unaffected, and the source stays 127.0.0.1
     * (out of FS's nat.auto ACL) so the Contact is never NAT-rewritten. */
    acc_cfg.proxy[acc_cfg.proxy_cnt++] = pj_str("sip:" FS_HOST ":5060");
    acc_cfg.cred_count = 1;
    acc_cfg.cred_info[0].realm = pj_str("*");
    acc_cfg.cred_info[0].scheme = pj_str("digest");
    acc_cfg.cred_info[0].username = pj_str(REG_USER);
    acc_cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    acc_cfg.cred_info[0].data = pj_str(REG_PASSWORD);
    /* Advertise 127.0.0.1 as the media address in SDP.  Two directions are
     * covered by this:
     *  - FS -> guest: FreeSWITCH sends RTP to our advertised 127.0.0.1:4000,
     *    which the slirp hostfwd udp::4000 forwards into the guest.  (slirp
     *    has no reverse route for 10.0.2.15, so we must NOT advertise that.)
     *  - guest -> FS: pjsua_media.c forces our outbound RTP to the slirp
     *    gateway 10.0.2.2.  slirp delivers it to the host stamped with
     *    source 127.0.0.1 (verified by a UDP probe), which matches our
     *    advertised c=, so FreeSWITCH's RTP source check accepts it. */
    acc_cfg.rtp_cfg.public_addr = pj_str("127.0.0.1");
    /* Retry failed REGISTERs every 5s so a transient failure recovers fast. */
    acc_cfg.reg_first_retry_interval = 5;
    acc_cfg.reg_retry_interval = 5;
    st = pjsua_acc_add(&acc_cfg, PJ_TRUE, &g_acc);
    if (st != PJ_SUCCESS) {
        printf("pj_phone: acc_add failed (%d)\r\n", st);
        pjsua_destroy();
        return -1;
    }
    printf("pj_phone: account added id=%d\r\n", g_acc);

    return 0;
}

/* Set the UI notification callback. */
void pj_phone_set_callback(pj_phone_cb_t cb, void *user_data) {
    g_cb = cb;
    g_cb_user = user_data;
}

/* Set the dial host (runtime override; no rebuild needed). */
void pj_phone_set_dial_host(const char *host, unsigned port) {
    taskENTER_CRITICAL();
    if (host != NULL && *host) {
        strncpy(g_dial_host, host, sizeof(g_dial_host) - 1);
        g_dial_host[sizeof(g_dial_host) - 1] = '\0';
    }
    g_dial_port = port;
    taskEXIT_CRITICAL();
    printf("pj_phone: dial host set to %s:%u\r\n", g_dial_host, g_dial_port);
}

/* Get the current dial host. */
const char *pj_phone_get_dial_host(void) {
    return g_dial_host;
}

/* Answer the pending incoming call. */
int pj_phone_answer(void) {
    return (int)phone_job_post(PHONE_JOB_ANSWER, NULL);
}

/* Reject the pending incoming call. */
int pj_phone_reject(void) {
    return (int)phone_job_post(PHONE_JOB_REJECT, NULL);
}

/* Hang up the active or ringing call. */
int pj_phone_hangup(void) {
    return (int)phone_job_post(PHONE_JOB_HANGUP, NULL);
}

/* Get the registration state. */
pj_phone_reg_state_t pj_phone_get_reg_state(void) {
    return g_reg_state;
}

/* Get the call state. */
pj_phone_call_state_t pj_phone_get_call_state(void) {
    return g_call_state;
}

/* Get the remote user. */
const char *pj_phone_get_peer_number(void) {
    return g_peer;
}

/* Get the call duration in ms (0 if not active). */
unsigned long pj_phone_get_call_duration_ms(void) {
    pj_time_val now = {0, 0};
    pj_time_val dur = {0, 0};

    if (g_call_state != PJ_PHONE_CALL_ACTIVE || g_call_start.sec == 0) {
        return 0;
    }
    pj_gettimeofday(&now);
    dur = now;
    PJ_TIME_VAL_SUB(dur, g_call_start);
    return (unsigned long)PJ_TIME_VAL_MSEC(dur);
}

/* Get the SIP status code of the last ended call. */
int pj_phone_get_last_call_status(void) {
    return g_last_call_status;
}

/* Get the text of the last ended call. */
const char *pj_phone_get_last_call_status_text(void) {
    return g_last_call_status_text;
}

/* Force re-registration under the current dial host. */
int pj_phone_reregister(void) {
    return (int)phone_job_post(PHONE_JOB_REREG, NULL);
}

/* Return 1 if inbound media has stalled while in a call. */
int pj_phone_get_media_stall(void) {
    return g_media_stall;
}

/* Get RTP stream packet counters for the active audio call (index 0). */
int pj_phone_get_stream_stats(unsigned *rx, unsigned *tx, unsigned *loss) {
    pjsua_stream_stat ss;
    if (g_call_id == PJSUA_INVALID_ID) {
        return -1;
    }
    if (pjsua_call_get_stream_stat(g_call_id, 0, &ss) != PJ_SUCCESS) {
        return -1;
    }
    if (rx)   *rx   = (unsigned)ss.rtcp.rx.pkt;
    if (tx)   *tx   = (unsigned)ss.rtcp.tx.pkt;
    if (loss) *loss = (unsigned)ss.rtcp.rx.loss;
    return 0;
}
