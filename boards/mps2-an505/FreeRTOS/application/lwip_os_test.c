/**
 * @file    lwip_os_test.c
 * @brief   lwIP test application in OS mode (NO_SYS=0 + FreeRTOS sys_arch).
 *
 * Built only when LWIP_OS=FreeRTOS.  Unlike the NO_SYS demo (lwip_test.c),
 * lwIP now runs with a dedicated tcpip_thread:
 *
 *   - tcpip_init() starts the tcpip_thread (priority 4, 16 KiB stack);
 *   - the LAN9118 netif is registered from the tcpip_thread context
 *     (netif->input = tcpip_input);
 *   - a lan9118_netif_thread task pumps RX frames into the tcpip_thread
 *     mailbox (the LAN9118 ISR only signals "RX available");
 *   - the ICMP ping test runs on the tcpip_thread via sys_timeout (raw API);
 *   - the external HTTP test uses the blocking sequential socket API
 *     from the lwIP OS task, demonstrating cross-thread lwIP usage.
 */
#include <stdint.h>
#include <string.h>

#include "printf.h"
#include "FreeRTOSConfig.h"
#include <FreeRTOS.h>
#include <task.h>

#include "lan9118.h"
#include "lan9118_netif.h"

#include "lwip/opt.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/raw.h"
#include "lwip/ip4_addr.h"
#include "lwip/prot/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/timeouts.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "netif/ethernet.h"

#if NO_SYS
#error "lwip_os_test.c requires NO_SYS=0 (LWIP_OS=FreeRTOS)"
#endif

#define LWIP_OS_TEST_ICMP_PING         (1U)
#define LWIP_OS_TEST_HTTP_SOCKET       (1U)

#define LWIP_OS_TASK_STACK             (4096U)
#define LWIP_OS_TASK_PRIO              (2U)
#define NETIF_THREAD_STACK             (1024U)
/* A/B test (2026-08-20): raising eth_rx above the media tasks (prio 5) did
 * NOT reduce dual-QEMU burst loss - it made it worse (steady 40-44/200 on
 * the peer side) by pre-empting tcpip_thread/media.  prio 3 is optimal. */
#define NETIF_THREAD_PRIO              (3U)

#if LWIP_OS_TEST_ICMP_PING
#define PING_DELAY_MS                  (1000U)
#define PING_DATA_SIZE                 (32U)
#define PING_ID                        (0x0A5FU)
#endif /* LWIP_OS_TEST_ICMP_PING */

#if LWIP_OS_TEST_HTTP_SOCKET
#define CONNECT_TEST_HOST              "www.baidu.com"
#define CONNECT_TEST_PORT              (80U)
#define CONNECT_TEST_INTERVAL_MS       (10000U)
#define CONNECT_TEST_RECV_TIMEOUT_MS   (5000U)
#define CONNECT_TEST_MAX_RX            (64U * 1024U)
#endif /* LWIP_OS_TEST_HTTP_SOCKET */

/* lwipopts.h routes LWIP_PLATFORM_ASSERT() here. */
void lwip_example_app_platform_assert(const char *msg, int line, const char *file) {
    printf("LWIP ASSERT %s:%d: %s\n", file, line, msg);
    while (true) {}
}

static struct netif s_netif = { 0 };
static volatile int s_tcpip_ready = 0;

#if LWIP_OS_TEST_ICMP_PING
static struct raw_pcb *s_ping_pcb;
static ip_addr_t s_ping_target;
static u16_t s_ping_seq = 0;
static volatile u32_t s_ping_sent = 0;
static volatile u32_t s_ping_recv = 0;

static void os_ping_prepare_echo(struct icmp_echo_hdr *iecho, u16_t len) {
    size_t i = 0;
    size_t data_len = (size_t)len - sizeof(struct icmp_echo_hdr);
    ICMPH_TYPE_SET(iecho, ICMP_ECHO);
    ICMPH_CODE_SET(iecho, 0);
    iecho->chksum = 0;
    iecho->id = PING_ID;
    iecho->seqno = lwip_htons((u16_t)(s_ping_seq + 1));
    for (i = 0; i < data_len; i++) {
        ((u8_t *)iecho)[sizeof(struct icmp_echo_hdr) + i] = (u8_t)i;
    }
    iecho->chksum = inet_chksum(iecho, len);
}

static void os_ping_send(void) {
    struct pbuf *p = NULL;
    struct icmp_echo_hdr *iecho = NULL;
    u16_t ping_size = (u16_t)(sizeof(struct icmp_echo_hdr) + PING_DATA_SIZE);
    p = pbuf_alloc(PBUF_TRANSPORT, ping_size, PBUF_RAM);
    if (p == NULL) {
        return;
    }
    iecho = (struct icmp_echo_hdr *)p->payload;
    os_ping_prepare_echo(iecho, ping_size);
    if (raw_sendto(s_ping_pcb, p, &s_ping_target) == ERR_OK) {
        s_ping_sent++;
        s_ping_seq++;
    }
    pbuf_free(p);
}

/* Called on the tcpip_thread when an ICMP packet arrives. */
static u8_t os_ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr) {
    struct ip_hdr *iphdr;
    struct icmp_echo_hdr *iecho;
    u8_t *icmp;
    (void)arg;
    (void)pcb;
    if (p->tot_len < ((u16_t)sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr))) {
        pbuf_free(p);
        return 1;
    }
    iphdr = (struct ip_hdr *)p->payload;
    if (IPH_PROTO(iphdr) != IP_PROTO_ICMP) {
        pbuf_free(p);
        return 1;
    }
    icmp = (u8_t *)p->payload + (u16_t)(IPH_HL(iphdr) * 4);
    if (p->tot_len < ((u16_t)(IPH_HL(iphdr) * 4) + sizeof(struct icmp_echo_hdr))) {
        pbuf_free(p);
        return 1;
    }
    iecho = (struct icmp_echo_hdr *)icmp;
    if ((iecho->id == PING_ID) && (iecho->seqno == lwip_htons(s_ping_seq))) {
        s_ping_recv++;
        printf("lwIP-OS ping: reply from %s seq=%u rtt_len=%u\n",
               ipaddr_ntoa(addr), (unsigned)lwip_ntohs(iecho->seqno),
               (unsigned)p->tot_len);
    }
    pbuf_free(p);
    return 1;
}

/* Runs on the tcpip_thread (scheduled via sys_timeout). */
static void os_ping_timeout(void *arg) {
    (void)arg;
    os_ping_send();
    sys_timeout(PING_DELAY_MS, os_ping_timeout, NULL);
}
#endif /* LWIP_OS_TEST_ICMP_PING */

#if LWIP_OS_TEST_HTTP_SOCKET
static const char s_conn_http_get[] =
    "GET / HTTP/1.1\r\n"
    "Host: www.baidu.com\r\n"
    "User-Agent: lwip-mps2-an505-os/1.0\r\n"
    "Connection: close\r\n"
    "\r\n";

static volatile u32_t s_http_ok = 0;
static volatile u32_t s_http_fail = 0;
static volatile u32_t s_http_rx = 0;

/* Blocking sequential (socket) API test, run from the lwIP OS task. */
static void os_http_test(void) {
    const struct hostent *he = NULL;
    struct sockaddr_in sa;
    int fd = -1;
    int total = 0;

    /* Blocking DNS resolve (uses the netconn API, sleeps this task). */
    he = lwip_gethostbyname(CONNECT_TEST_HOST);
    if (he == NULL) {
        printf("lwIP-OS http: DNS failed for %s\n", CONNECT_TEST_HOST);
        s_http_fail++;
        return;
    }
    printf("lwIP-OS http: DNS %s -> %d.%d.%d.%d\n", CONNECT_TEST_HOST,
           (int)((u8_t *)he->h_addr)[0], (int)((u8_t *)he->h_addr)[1],
           (int)((u8_t *)he->h_addr)[2], (int)((u8_t *)he->h_addr)[3]);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = lwip_htons(CONNECT_TEST_PORT);
    memcpy(&sa.sin_addr, he->h_addr, (size_t)he->h_length);

    fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("lwIP-OS http: socket failed\n");
        s_http_fail++;
        return;
    }
    {
        struct timeval tv;
        tv.tv_sec = CONNECT_TEST_RECV_TIMEOUT_MS / 1000;
        tv.tv_usec = (long)((CONNECT_TEST_RECV_TIMEOUT_MS % 1000) * 1000);
        lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    if (lwip_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        printf("lwIP-OS http: connect to %s:%u failed\n",
               CONNECT_TEST_HOST, (unsigned)CONNECT_TEST_PORT);
        lwip_close(fd);
        s_http_fail++;
        return;
    }
    printf("lwIP-OS http: connected to %s:%u\n",
           CONNECT_TEST_HOST, (unsigned)CONNECT_TEST_PORT);

    if (lwip_send(fd, s_conn_http_get, (size_t)(sizeof(s_conn_http_get) - 1U), 0) < 0) {
        printf("lwIP-OS http: send failed\n");
        lwip_close(fd);
        s_http_fail++;
        return;
    }
    {
        char buf[256];
        int n;
        /* Read until the server closes (Connection: close), bounded by a
           receive cap so a chatty peer cannot stall the task forever. */
        while ((int)total < (int)CONNECT_TEST_MAX_RX) {
            n = lwip_recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break; /* 0 = peer closed, -1 = recv timeout */
            }
            total += n;
        }
        s_http_rx += (u32_t)total;
    }
    if (total > 0) {
        s_http_ok++;
        printf("lwIP-OS http: got %u bytes (OK), total_rx=%lu\n",
               (unsigned)total, (unsigned long)s_http_rx);
    } else {
        s_http_fail++;
        printf("lwIP-OS http: recv timeout/no data\n");
    }
    lwip_close(fd);
}
#endif /* LWIP_OS_TEST_HTTP_SOCKET */

/* Runs on the tcpip_thread right after lwIP initialisation. */
static void tcpip_init_done(void *arg) {
    struct netif *n = (struct netif *)arg;
    ip4_addr_t ipaddr = { 0 }, netmask = { 0 }, gw = { 0 };
    uint8_t mac[6] = { 0 };

    /* slirp: each instance is its own NAT; hostfwd injects to 10.0.2.15,
     * so both guests must use .15 (see WORKLOG-2026-08-20-netdev-socket.md). */
    IP4_ADDR(&ipaddr, 10, 0, 2, 15);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);
    netif_add(n, &ipaddr, &netmask, &gw, NULL, lan9118_netif_init, tcpip_input);
    netif_set_default(n);
    netif_set_up(n);

    /* DNS server (QEMU slirp). */
    {
        ip4_addr_t dns4 = { 0 };
        ip_addr_t dns = { 0 };
        IP4_ADDR(&dns4, 10, 0, 2, 3);
        ip_addr_copy_from_ip4(dns, dns4);
        dns_setserver(0, &dns);
    }

#if LWIP_OS_TEST_ICMP_PING
    /* Periodic ping of the QEMU user-mode gateway (10.0.2.2). */
    ip_2_ip4(&s_ping_target)->addr = lwip_htonl(LWIP_MAKEU32(10, 0, 2, 2));
    IP_SET_TYPE(&s_ping_target, IPADDR_TYPE_V4);
    s_ping_pcb = raw_new(IP_PROTO_ICMP);
    if (s_ping_pcb != NULL) {
        raw_recv(s_ping_pcb, os_ping_recv, NULL);
        raw_bind(s_ping_pcb, IP_ADDR_ANY);
        sys_timeout(PING_DELAY_MS, os_ping_timeout, NULL);
    } else {
        printf("lwIP-OS: ping init failed (raw_new)\n");
    }
#endif /* LWIP_OS_TEST_ICMP_PING */

    lan9118_get_mac(mac);
    {
        char ipbuf[16], gwbuf[16];
        printf("lwIP-OS up: IP %s gw %s MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
               ip4addr_ntoa_r(&ipaddr, ipbuf, sizeof(ipbuf)),
               ip4addr_ntoa_r(&gw, gwbuf, sizeof(gwbuf)),
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    s_tcpip_ready = 1;
}

static void lwip_os_task_entry(void *parameters) {
    u32_t last_report = 0;
    (void)parameters;

    /* Start lwIP in OS mode: initialises the stack and spawns tcpip_thread. */
    tcpip_init(tcpip_init_done, &s_netif);

    /* Pump LAN9118 RX into the tcpip_thread (ISR only signals RX available). */
    xTaskCreate(lan9118_netif_thread, "eth_rx", NETIF_THREAD_STACK,
                &s_netif, NETIF_THREAD_PRIO, NULL);

    while (!s_tcpip_ready) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

#if defined(PJ_DUAL_ROLE_CALLER) || defined(PJ_DUAL_ROLE_CALLEE)
    /* Dual-QEMU call mode: keep the network up (tcpip + eth_rx) but skip the
     * HTTP-to-internet / ping self-tests.  A/B test (2026-08-20) showed they
     * WORSEN the slirp forwarding loss (with them ON, 2/6 directions dropped
     * 64/142 of 200 frames vs near-zero with them OFF), so keep them off
     * during the dual call regression. */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

#if LWIP_OS_TEST_HTTP_SOCKET
    os_http_test();
#endif

    while (true) {
        if (((u32_t)xTaskGetTickCount() - last_report) >= 5000U) {
            last_report = (u32_t)xTaskGetTickCount();
#if LWIP_OS_TEST_ICMP_PING
            printf("lwIP-OS: ping sent=%lu recv=%lu ",
                   (unsigned long)s_ping_sent, (unsigned long)s_ping_recv);
#endif
#if LWIP_OS_TEST_HTTP_SOCKET
            printf("http ok=%lu fail=%lu rx=%lu ",
                   (unsigned long)s_http_ok, (unsigned long)s_http_fail,
                   (unsigned long)s_http_rx);
#endif
            printf("link=%d\n", lan9118_link_status());
        }
#if LWIP_OS_TEST_HTTP_SOCKET
        vTaskDelay(pdMS_TO_TICKS(CONNECT_TEST_INTERVAL_MS));
        os_http_test();
#else
        vTaskDelay(pdMS_TO_TICKS(1000));
#endif
    }
}

void lwip_os_task_init(void) {
    static TaskHandle_t lwip_os_task = NULL;
    xTaskCreate(lwip_os_task_entry, "lwip_os_task", LWIP_OS_TASK_STACK,
                NULL, LWIP_OS_TASK_PRIO, &lwip_os_task);
}
