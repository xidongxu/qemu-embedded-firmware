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
#include <pjsua-lib/pjsua.h>

#define HOST_GW        "10.0.2.2"   /* slirp gateway = host loopback alias */
#define HOST_SIP_PORT  5060         /* host pjsua / SIP server UDP port */
#define GUEST_SIP_PORT 15062        /* guest SIP port (hostfwd'd as itself) */
#define DIAL_TARGET    "sip:user@10.0.2.2:5060"

static pjsua_acc_id g_acc = PJSUA_INVALID_ID;

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
    printf("pj_phone: incoming call %d, answering 200\r\n", call_id);
    pjsua_call_answer(call_id, 200, NULL, NULL);
}

static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    pjsua_call_info ci;
    PJ_UNUSED_ARG(e);
    if (pjsua_call_get_info(call_id, &ci) == PJ_SUCCESS) {
        printf("pj_phone: call %d state=%d (%.*s)\r\n", call_id,
               (int)ci.state, (int)ci.state_text.slen, ci.state_text.ptr);
    }
}

static void on_call_media_state(pjsua_call_id call_id)
{
    pjsua_call_info ci;
    if (pjsua_call_get_info(call_id, &ci) == PJ_SUCCESS) {
        printf("pj_phone: call %d media_status=%d%s\r\n", call_id,
               (int)ci.media_status,
               ci.media_status == PJSUA_CALL_MEDIA_ACTIVE ? " (ACTIVE)" : "");
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
    pj_str_t uri;
    pjsua_transport_id tp;
    pjsua_call_id call_id;
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
    log_cfg.console_level = 6;

    pjsua_media_config_default(&media_cfg);
    media_cfg.clock_rate = 8000;
    media_cfg.snd_clock_rate = 8000;
    media_cfg.channel_count = 1;
    media_cfg.ec_options = 0;

    st = pjsua_init(&cfg, &log_cfg, &media_cfg);
    if (st != PJ_SUCCESS) {
        printf("pj_phone: pjsua_init failed (%d)\r\n", st);
        return -1;
    }
    printf("pj_phone: pjsua_init OK\r\n");

    /* No sound card: use null audio device. */
    st = pjsua_set_null_snd_dev();
    printf("pj_phone: pjsua_set_null_snd_dev -> %d\r\n", st);

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
    pj_strset2(&uri, DIAL_TARGET);
    st = pjsua_call_make_call(g_acc, &uri, NULL, NULL, NULL, &call_id);
    printf("pj_phone: make_call(%s) -> %d (call=%d)\r\n", DIAL_TARGET,
           (int)st, (int)call_id);
    if (st != PJ_SUCCESS)
        return -1;

    return 0;
}
