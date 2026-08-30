/*
 * phone_net.c - UDP command server for the PJ_PHONE app.
 *
 * Lets a host drive the phone over the slirp network via hostfwd
 * (udp::15000-:15000), bypassing the UART entirely.  Commands are single
 * lines; the response is sent back over UDP.
 *
 * Uses blocking recvfrom in a dedicated task (no SO_RCVTIMEO, which can
 * hang on this lwIP port - see pj_sip_dual_test.c notes).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include "pj/os.h"
#include "pj_phone.h"
#include "tlsf_port.h"
#include "FreeRTOS.h"
#include "task.h"

#define PHONE_CMD_PORT 15000

static const char *pnet_call_state(pj_phone_call_state_t st)
{
    switch (st) {
    case PJ_PHONE_CALL_DIALING:  return "DIALING";
    case PJ_PHONE_CALL_INCOMING: return "INCOMING";
    case PJ_PHONE_CALL_ACTIVE:   return "ACTIVE";
    default:                     return "IDLE";
    }
}

static void pnet_exec(const char *line, char *resp, int rsize)
{
    char tmp[128];
    char *cmd;
    char *arg;
    int rc = -1;

    snprintf(tmp, sizeof(tmp), "%s", line);
    cmd = strtok(tmp, " \t");
    if (cmd == NULL) {
        snprintf(resp, rsize, "ERR empty");
        return;
    }
    arg = strtok(NULL, " \t");

    if (strcmp(cmd, "dial") == 0) {
        if (arg != NULL) {
            rc = pj_phone_dial(arg);
            snprintf(resp, rsize, "dial %s rc=%d", arg, rc);
        } else {
            snprintf(resp, rsize, "ERR dial needs number");
        }
    } else if (strcmp(cmd, "hangup") == 0) {
        rc = pj_phone_hangup();
        snprintf(resp, rsize, "hangup rc=%d", rc);
    } else if (strcmp(cmd, "answer") == 0) {
        rc = pj_phone_answer();
        snprintf(resp, rsize, "answer rc=%d", rc);
    } else if (strcmp(cmd, "reject") == 0) {
        rc = pj_phone_reject();
        snprintf(resp, rsize, "reject rc=%d", rc);
    } else if (strcmp(cmd, "dtmf") == 0) {
        if (arg != NULL) {
            rc = pj_phone_send_dtmf(arg);
            snprintf(resp, rsize, "dtmf %s rc=%d", arg, rc);
        } else {
            snprintf(resp, rsize, "ERR dtmf needs digits");
        }
    } else if (strcmp(cmd, "stat") == 0) {
        unsigned rx = 0, tx = 0, loss = 0;
        if (pj_phone_get_stream_stats(&rx, &tx, &loss) == 0) {
            snprintf(resp, rsize, "rx=%u tx=%u loss=%u", rx, tx, loss);
        } else {
            snprintf(resp, rsize, "rx=-1 (no active call)");
        }
    } else if (strcmp(cmd, "memp") == 0) {
        /* lwIP pool usage (diagnostic for the 120105 socket leak). */
        extern struct stats_ lwip_stats;
        snprintf(resp, rsize,
                 "mem.used=%u/%u max=%u err=%u | UDP_PCB %u/%u err=%u | "
                 "NETCONN %u/%u err=%u | NETBUF %u/%u err=%u | "
                 "PBUF %u/%u err=%u | UDP xmit=%u recv=%u",
                 (unsigned)lwip_stats.mem.used, (unsigned)MEM_SIZE,
                 (unsigned)lwip_stats.mem.max, (unsigned)lwip_stats.mem.err,
                 (unsigned)lwip_stats.memp[MEMP_UDP_PCB]->used,
                 (unsigned)MEMP_NUM_UDP_PCB,
                 (unsigned)lwip_stats.memp[MEMP_UDP_PCB]->err,
                 (unsigned)lwip_stats.memp[MEMP_NETCONN]->used,
                 (unsigned)MEMP_NUM_NETCONN,
                 (unsigned)lwip_stats.memp[MEMP_NETCONN]->err,
                 (unsigned)lwip_stats.memp[MEMP_NETBUF]->used,
                 (unsigned)MEMP_NUM_NETBUF,
                 (unsigned)lwip_stats.memp[MEMP_NETBUF]->err,
                 (unsigned)lwip_stats.memp[MEMP_PBUF]->used,
                 (unsigned)MEMP_NUM_PBUF,
                 (unsigned)lwip_stats.memp[MEMP_PBUF]->err,
                 (unsigned)lwip_stats.udp.xmit,
                 (unsigned)lwip_stats.udp.recv);
    } else if (strcmp(cmd, "mem") == 0) {
        /* Unified TLSF allocator usage (used/free/min-free/total). */
        snprintf(resp, rsize, "tlsf pool=%u used=%u free=%u minfree=%u",
                 (unsigned)tlsf_port_get_total_size(),
                 (unsigned)tlsf_port_get_used_size(),
                 (unsigned)tlsf_port_get_free_size(),
                 (unsigned)tlsf_port_get_min_free_size());
    } else if (strcmp(cmd, "status") == 0) {
        snprintf(resp, rsize,
                 "reg=%d call=%s peer=%s dur=%lu last=%d(%s) stall=%d host=%s",
                 (int)pj_phone_get_reg_state(),
                 pnet_call_state(pj_phone_get_call_state()),
                 pj_phone_get_peer_number(),
                 (unsigned long)pj_phone_get_call_duration_ms(),
                 pj_phone_get_last_call_status(),
                 pj_phone_get_last_call_status_text(),
                 pj_phone_get_media_stall(),
                 pj_phone_get_dial_host());
    } else if (strcmp(cmd, "host") == 0) {
        char *p = strtok(NULL, " \t");
        if (arg != NULL && p != NULL) {
            pj_phone_set_dial_host(arg, (unsigned)atoi(p));
            snprintf(resp, rsize, "host %s:%s", arg, p);
        } else {
            snprintf(resp, rsize, "ERR host <ip> <port>");
        }
    } else if (strcmp(cmd, "rereg") == 0) {
        rc = pj_phone_reregister();
        snprintf(resp, rsize, "rereg rc=%d", rc);
    } else if (strcmp(cmd, "udptest") == 0) {
        /* Send a UDP probe to the host (slirp gateway 10.0.2.2) so the host
         * can observe which source IP slirp stamps when it forwards
         * guest->host packets (127.0.0.1 vs 10.0.2.15), and on which target
         * IP the host receives it. Optional arg = target port (def 5005). */
        struct sockaddr_in dst;
        int us = socket(AF_INET, SOCK_DGRAM, 0);
        int usrc = -1;
        int dport = (arg != NULL) ? atoi(arg) : 5005;
        if (us >= 0) {
            memset(&dst, 0, sizeof(dst));
            dst.sin_family = AF_INET;
            dst.sin_addr.s_addr = inet_addr("10.0.2.2");
            dst.sin_port = htons((unsigned short)dport);
            usrc = sendto(us, "hello-from-guest", 16, 0,
                          (struct sockaddr *)&dst, sizeof(dst));
            closesocket(us);
        }
        snprintf(resp, rsize, "udptest %d sendto=%d", dport, usrc);
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        snprintf(resp, rsize,
                 "cmds: dial <ext> | hangup | answer | reject | dtmf <d> | "
                 "status | host <ip> <port> | rereg");
    } else {
        snprintf(resp, rsize, "ERR unknown '%s'", cmd);
    }
}

void phone_net_task(void *arg)
{
    struct sockaddr_in addr;
    struct sockaddr_in from;
    socklen_t flen;
    char buf[200];
    char resp[512];
    int sock;
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(800));

    /* Register this FreeRTOS task with PJLIB so pjsua calls (stream stat)
     * from this thread don't hit the NULL-thread PJSUA_LOCK spin. */
    pj_thread_desc desc;
    pj_thread_t *thr;
    pj_thread_register("netcmd", desc, &thr);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        printf("phone_net: socket() failed\r\n");
        vTaskDelete(NULL);
        return;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PHONE_CMD_PORT);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("phone_net: bind :%d failed\r\n", PHONE_CMD_PORT);
        closesocket(sock);
        vTaskDelete(NULL);
        return;
    }
    printf("phone_net: UDP cmd server on :%d\r\n", PHONE_CMD_PORT);

    for (;;) {
        flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &flen);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        buf[n] = '\0';
        while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) {
            buf[--n] = '\0';
        }
        resp[0] = '\0';
        pnet_exec(buf, resp, sizeof(resp));
        printf("phone_net: cmd '%s' -> %s\r\n", buf, resp);
        sendto(sock, resp, (size_t)strlen(resp), 0,
               (struct sockaddr *)&from, flen);
    }
}
