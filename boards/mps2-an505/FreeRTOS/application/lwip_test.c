/**
 * @file    lwip_test.c
 * @brief   lwIP test application (NO_SYS mode) for the LAN9118 driver.
 *
 * Structured like lv_disp.c: a dedicated FreeRTOS task (lwip_task_init /
 * lwip_task_entry) that
 *   - initialises the lwIP stack,
 *   - brings up the LAN9118 netif (static IP in the QEMU user-mode
 *     subnet 10.0.2.0/24),
 *   - runs a periodic ICMP ping of the QEMU gateway (10.0.2.2) as a
 *     smoke test of the full TX / ARP / RX / IP / ICMP path,
 *   - pumps the netif (RX) and lwIP timers in the NO_SYS main loop.
 *
 * Because lwIP is built with NO_SYS=1, all lwIP processing happens in
 * this single task; the LAN9118 ISR only signals "RX available" and the
 * netif is drained here via lan9118_netif_poll().
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
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/icmp.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/inet_chksum.h"
#include "lwip/timeouts.h"
#include "lwip/sys.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "netif/ethernet.h"

/* Monotonic millisecond time for the lwIP timer engine. */
u32_t sys_now(void) {
    return (u32_t)xTaskGetTickCount();
}

/* Some lwIP modules (e.g. PPP) use a jiffies counter. */
#ifndef sys_jiffies
u32_t sys_jiffies(void) {
    return sys_now();
}
#endif

/* lwipopts.h routes LWIP_PLATFORM_ASSERT() here. */
void lwip_example_app_platform_assert(const char *msg, int line, const char *file) {
    printf("LWIP ASSERT %s:%d: %s\n", file, line, msg);
    while(true) {}
}

#define LWIP_TEST_ICMP_PING         (1U)
#define LWIP_TEST_EXTERNAL_CONNECT  (1U)

#define LWIP_TASK_STACK             (4096U)
#define LWIP_TASK_PRIO              (2U)

#if LWIP_TEST_ICMP_PING
#define PING_DELAY_MS               (1000U)
#define PING_DATA_SIZE              (32U)
#define PING_ID                     (0xAFAFU)
#endif /* LWIP_TEST_ICMP_PING */

#if LWIP_TEST_EXTERNAL_CONNECT
#define CONNECT_TEST_HOST           "www.baidu.com"
#define CONNECT_TEST_PORT           (80U)
/* re-run DNS + TCP every 10 s */
#define CONNECT_TEST_INTERVAL_MS    (10000U)
#endif /* LWIP_TEST_EXTERNAL_CONNECT */

static struct netif s_netif = { 0 };

#if LWIP_TEST_ICMP_PING
static struct raw_pcb *s_ping_pcb;
static ip_addr_t s_ping_target;
static u16_t s_ping_seq = 0;
static volatile u32_t s_ping_sent = 0;
static volatile u32_t s_ping_recv = 0;
#endif /* LWIP_TEST_ICMP_PING */

#if LWIP_TEST_EXTERNAL_CONNECT
static struct tcp_pcb *s_conn_pcb;
static ip_addr_t s_conn_target;
static volatile u32_t s_conn_dns_ok = 0;
static volatile u32_t s_conn_tcp_ok = 0;
static volatile u32_t s_conn_tcp_fail = 0;
static u32_t s_conn_rx_bytes = 0;
static u8_t s_conn_busy = 0;
#endif /* LWIP_TEST_EXTERNAL_CONNECT */

#if LWIP_TEST_ICMP_PING
static void ping_prepare_echo(struct icmp_echo_hdr *iecho, u16_t len) {
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

static void ping_send(struct raw_pcb *pcb, const ip_addr_t *addr) {
    struct pbuf *p = NULL;
    struct icmp_echo_hdr *iecho = NULL;
    u16_t ping_size = (u16_t)(sizeof(struct icmp_echo_hdr) + PING_DATA_SIZE);
    p = pbuf_alloc(PBUF_TRANSPORT, ping_size, PBUF_RAM);
    if (p == NULL) {
        return;
    }
    iecho = (struct icmp_echo_hdr *)p->payload;
    ping_prepare_echo(iecho, ping_size);
    if (raw_sendto(pcb, p, addr) == ERR_OK) {
        s_ping_sent++;
        s_ping_seq++;
    }
    pbuf_free(p);
}

/* Called (from the netif input path) when an ICMP packet arrives. */
static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    struct ip_hdr *iphdr;
    struct icmp_echo_hdr *iecho;
    u8_t *icmp;
    (void)arg;
    (void)pcb;
    if (p->tot_len < ((u16_t)sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr))) {
        pbuf_free(p);
        return 1;
    }
    /* For raw sockets the pbuf payload starts at the IP header. */
    iphdr = (struct ip_hdr *)p->payload;
    if (IPH_PROTO(iphdr) != IP_PROTO_ICMP) {
        pbuf_free(p);
        return 1;
    }
    icmp = (u8_t *)p->payload + (u16_t)(IPH_HL(iphdr) * 4);
    if (p->tot_len < ((u16_t)(IPH_HL(iphdr) * 4) +
                      sizeof(struct icmp_echo_hdr))) {
        pbuf_free(p);
        return 1;
    }
    iecho = (struct icmp_echo_hdr *)icmp;
    if ((iecho->id == PING_ID) && (iecho->seqno == lwip_htons(s_ping_seq))) {
        s_ping_recv++;
        printf("lwIP ping: reply from %s seq=%u rtt_len=%u\n",
               ipaddr_ntoa(addr),
               (unsigned)lwip_ntohs(iecho->seqno),
               (unsigned)p->tot_len);
    }
    pbuf_free(p);
    return 1;
}

static void ping_timeout(void *arg) {
    struct raw_pcb *pcb = (struct raw_pcb *)arg;
    ping_send(pcb, &s_ping_target);
    sys_timeout(PING_DELAY_MS, ping_timeout, pcb);
}
#endif /* LWIP_TEST_ICMP_PING */

#if LWIP_TEST_EXTERNAL_CONNECT
static const char s_conn_http_get[] =
    "GET / HTTP/1.1\r\n"
    "Host: www.baidu.com\r\n"
    "User-Agent: lwip-mps2-an505/1.0\r\n"
    "Connection: close\r\n"
    "\r\n";

static u16_t s_conn_print_left = 0;

static void conn_tcp_err(void *arg, err_t err) {
    (void)arg;
    s_conn_tcp_fail++;
    s_conn_busy = 0;
    s_conn_pcb = NULL;
    printf("connect-test: TCP error (%d)\n", (int)err);
}

static err_t conn_tcp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    (void)err;
    if (p == NULL) {
        printf("connect-test: server closed, rx=%lu bytes (OK)\n", (unsigned long)s_conn_rx_bytes);
        s_conn_tcp_ok++;
        s_conn_busy = 0;
        s_conn_pcb = NULL;
        tcp_close(tpcb);
        return ERR_OK;
    }
    s_conn_rx_bytes += p->tot_len;
    if (s_conn_print_left > 0) {
        static char dbg[160];
        u16_t n = 0;
        struct pbuf *q = p;
        u16_t budget = s_conn_print_left;

        while ((q != NULL) && (budget > 0) && (n < (u16_t)(sizeof(dbg) - 1))) {
            u16_t i;
            u16_t m = q->len;

            if (m > budget) {
                m = budget;
            }
            if (m > ((u16_t)sizeof(dbg) - 1 - n)) {
                m = (u16_t)((u16_t)sizeof(dbg) - 1 - n);
            }
            for (i = 0; i < m; i++) {
                char c = (char)((u8_t *)q->payload)[i];
                dbg[n++] = (c >= 0x20 && c <= 0x7E) ? c : '.';
            }
            budget = (u16_t)(budget - m);
            q = q->next;
        }
        dbg[n] = '\0';
        printf("connect-test: RX[%u]: %s\n", (unsigned)n, dbg);
        s_conn_print_left = 0;
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t conn_tcp_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        conn_tcp_err(NULL, err);
        return ERR_OK;
    }
    printf("connect-test: TCP connected to %s:%u\n", ipaddr_ntoa(&s_conn_target), (unsigned)CONNECT_TEST_PORT);
    s_conn_rx_bytes = 0;
    s_conn_print_left = 256;
    if (tcp_write(tpcb, s_conn_http_get, (u16_t)(sizeof(s_conn_http_get) - 1U), TCP_WRITE_FLAG_COPY) == ERR_OK) {
        tcp_output(tpcb);
    }
    return ERR_OK;
}

static void conn_test_connect(void) {
    if (s_conn_pcb != NULL) {
        return;
    }
    s_conn_pcb = tcp_new();
    if (s_conn_pcb == NULL) {
        printf("connect-test: tcp_new failed\n");
        s_conn_busy = 0;
        return;
    }
    tcp_arg(s_conn_pcb, NULL);
    tcp_recv(s_conn_pcb, conn_tcp_recv);
    tcp_err(s_conn_pcb, conn_tcp_err);
    if (tcp_connect(s_conn_pcb, &s_conn_target, CONNECT_TEST_PORT, conn_tcp_connected) != ERR_OK) {
        printf("connect-test: tcp_connect failed\n");
        tcp_abort(s_conn_pcb);
        s_conn_pcb = NULL;
        s_conn_busy = 0;
    }
}

static void conn_dns_found(const char *name, const ip_addr_t *ipaddr, void *callback_arg) {
    (void)name;
    (void)callback_arg;
    if (ipaddr == NULL) {
        printf("connect-test: DNS failed for %s\n", CONNECT_TEST_HOST);
        s_conn_busy = 0;
        return;
    }
    ip_addr_copy(s_conn_target, *ipaddr);
    s_conn_dns_ok++;
    printf("connect-test: DNS %s -> %s\n", CONNECT_TEST_HOST, ipaddr_ntoa(ipaddr));
    conn_test_connect();
}

static void conn_test_start(void) {
    err_t res = 0;
    s_conn_busy = 1;
    res = dns_gethostbyname(CONNECT_TEST_HOST, &s_conn_target, conn_dns_found, NULL);
    if (res == ERR_OK) {
        conn_dns_found(CONNECT_TEST_HOST, &s_conn_target, NULL);
    }
}

static void conn_test_timeout(void *arg) {
    (void)arg;
    if (!s_conn_busy) {
        conn_test_start();
    }
    sys_timeout(CONNECT_TEST_INTERVAL_MS, conn_test_timeout, NULL);
}
#endif /* LWIP_TEST_EXTERNAL_CONNECT */

static void lwip_task_entry(void *parameters) {
    ip4_addr_t ipaddr = { 0 }, netmask = { 0 }, gw = { 0 };
    uint8_t mac[6] = { 0 };
    u32_t last_report = 0;
    (void)parameters;
    lwip_init();
    /* Static IP in the QEMU user-mode subnet 10.0.2.0/24. */
    IP4_ADDR(&ipaddr, 10, 0, 2, 15);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);
    netif_add(&s_netif, &ipaddr, &netmask, &gw, NULL, lan9118_netif_init, ethernet_input);
    netif_set_default(&s_netif);
    netif_set_up(&s_netif);
#if LWIP_TEST_ICMP_PING
    /* Periodic ping of the QEMU user-mode gateway (10.0.2.2). */
    ip_2_ip4(&s_ping_target)->addr = lwip_htonl(LWIP_MAKEU32(10, 0, 2, 2));
    IP_SET_TYPE(&s_ping_target, IPADDR_TYPE_V4);
    s_ping_pcb = raw_new(IP_PROTO_ICMP);
    if (s_ping_pcb != NULL) {
        raw_recv(s_ping_pcb, ping_recv, NULL);
        raw_bind(s_ping_pcb, IP_ADDR_ANY);
        sys_timeout(PING_DELAY_MS, ping_timeout, s_ping_pcb);
    } else {
        printf("lwIP: ping init failed (raw_new)\n");
    }
#endif /* LWIP_TEST_ICMP_PING */

#if LWIP_TEST_EXTERNAL_CONNECT
    {
        ip4_addr_t dns4 = { 0 };
        ip_addr_t dns = { 0 };
        IP4_ADDR(&dns4, 10, 0, 2, 3);
        ip_addr_copy_from_ip4(dns, dns4);
        dns_setserver(0, &dns);
    }
    conn_test_start();
    sys_timeout(CONNECT_TEST_INTERVAL_MS, conn_test_timeout, NULL);
#endif /* LWIP_TEST_EXTERNAL_CONNECT */

    lan9118_get_mac(mac);
    {
        char ipbuf[16], gwbuf[16];
        printf("lwIP up: IP %s gw %s MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
               ip4addr_ntoa_r(&ipaddr, ipbuf, sizeof(ipbuf)),
               ip4addr_ntoa_r(&gw, gwbuf, sizeof(gwbuf)),
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    while (true) {
        /* Drain RX + monitor link (NO_SYS main loop). */
        lan9118_netif_poll(&s_netif);
        /* Run lwIP timers (ping timeout, ARP aging, ...). */
        sys_check_timeouts();

        if (((u32_t)xTaskGetTickCount() - last_report) >= 5000U) {
            last_report = (u32_t)xTaskGetTickCount();
#if LWIP_TEST_ICMP_PING
            printf("lwIP: ping sent=%lu recv=%lu ",
                   (unsigned long)s_ping_sent,
                   (unsigned long)s_ping_recv);
#endif /* LWIP_TEST_ICMP_PING */
#if LWIP_TEST_EXTERNAL_CONNECT
            printf("connect: dns_ok=%lu tcp_ok=%lu tcp_fail=%lu rx=%lu ",
                   (unsigned long)s_conn_dns_ok,
                   (unsigned long)s_conn_tcp_ok,
                   (unsigned long)s_conn_tcp_fail,
                   (unsigned long)s_conn_rx_bytes);
#endif /* LWIP_TEST_EXTERNAL_CONNECT */
            printf("link=%d\n", lan9118_link_status());
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void lwip_task_init(void) {
    static TaskHandle_t lwip_task = NULL;
    xTaskCreate(lwip_task_entry, "lwip_task", LWIP_TASK_STACK, NULL, LWIP_TASK_PRIO, &lwip_task);
}
