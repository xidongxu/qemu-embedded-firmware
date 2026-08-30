/*
 * sntp_sync.c - lwIP SNTP client wrapper that maintains the guest RTC epoch.
 *
 * Server: host tap0 address (172.16.23.1:12345).  Run the host-side NTP
 * server works/tools/sntp_server.py so the guest can sync.  mbedtls_port.c
 * uses sntp_sync_get_epoch() as the real clock base; before the first sync
 * it falls back to the fixed BOOT_EPOCH.
 */
#include "sntp_sync.h"
#include "printf.h"

#include <string.h>

#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#include "lwip/tcpip.h"
#include "lwip/apps/sntp.h"

/* NTP server: host tap0 address (see works/tools/sntp_server.py). */
#define SNTP_SERVER_IP "172.16.23.1"

static volatile time_t s_rtc_epoch = 0;

void sntp_sync_set_system_time(unsigned int sec)
{
    s_rtc_epoch = (time_t) sec;
    printf("sntp_sync: RTC epoch synced to %u\r\n", sec);
}

time_t sntp_sync_get_epoch(void)
{
    return s_rtc_epoch;
}

/* Runs on the tcpip thread: sntp_setserver/init require the core lock
 * (LWIP_ASSERT_CORE_LOCKED), so they cannot run directly on main_task. */
static void sntp_sync_start(void *arg)
{
    ip_addr_t srv;

    LWIP_UNUSED_ARG(arg);
    memset(&srv, 0, sizeof(srv));
    srv.type = IPADDR_TYPE_V4;
    srv.u_addr.ip4.addr = ipaddr_addr(SNTP_SERVER_IP);
    sntp_setserver(0, &srv);
    sntp_init();
}

/* Post the SNTP startup to the tcpip thread (safe from any task). */
void sntp_sync_init(void)
{
    tcpip_callback(sntp_sync_start, NULL);
}
