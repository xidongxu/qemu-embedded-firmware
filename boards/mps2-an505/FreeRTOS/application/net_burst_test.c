/*
 * net_burst_test.c - bare network burst test (NO pjproject).
 *
 * Verifies the underlying network path used by the dual-QEMU call:
 * LAN9118 driver + lwIP socket layer + QEMU "socket" netdev
 * (point-to-point, no slirp).  Both instances exchange 200 UDP packets
 * at a 10 ms pace (mimicking RTP frames) and report per direction:
 *   - received / lost count (loss %)
 *   - out-of-order packets
 *   - arrival interval min/avg/max (FreeRTOS tick, 1 ms)
 *
 * This isolates whether the packet loss observed inside pjmedia is a
 * BASE-NETWORK problem (this test shows it too) or a pjproject
 * integration problem (base network clean, loss only appears in the
 * pjmedia path).
 *
 * Topology (dual QEMU, socket netdev):
 *   caller 10.0.2.15 -> callee 10.0.2.16:20001
 *   callee 10.0.2.16 -> caller 10.0.2.15:20002
 */
#ifndef PJ_DUAL_ROLE_CALLER
#ifndef PJ_DUAL_ROLE_CALLEE
#error "define PJ_DUAL_ROLE_CALLER or PJ_DUAL_ROLE_CALLEE"
#endif
#endif

#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "printf.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define N_PKTS           200
#define PKT_INTERVAL_MS  10
#define RX_TIMEOUT_MS    2000

#if defined(PJ_DUAL_ROLE_CALLER)
#  define ROLE_NAME     "caller"
#  define MY_IP         "10.0.2.15"
#  define PEER_IP       "10.0.2.16"
#  define MY_RX_PORT    20002
#  define PEER_RX_PORT  20001
#else
#  define ROLE_NAME     "callee"
#  define MY_IP         "10.0.2.16"
#  define PEER_IP       "10.0.2.15"
#  define MY_RX_PORT    20001
#  define PEER_RX_PORT  20002
#endif

typedef struct {
    uint32_t seq;
    uint32_t tx_tick;
} burst_pkt_t;

#define READY_MAGIC  0xFFFFFFFFu   /* handshake seq: both sides start together */

static int s_fd = -1;
static volatile int g_sent;
static volatile int g_rx_count;
static volatile int g_ooo;
static volatile int g_done;
static volatile int g_min_int, g_max_int;
static volatile long g_sum_int;
static uint8_t  g_rx_seen[N_PKTS];
static uint32_t g_rx_arr[N_PKTS];

static void sender_task(void *arg)
{
    burst_pkt_t p;
    struct sockaddr_in peer;
    int i;
    (void)arg;

    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons((u16_t)PEER_RX_PORT);
    peer.sin_addr.s_addr = inet_addr(PEER_IP);

    for (i = 0; i < N_PKTS; i++) {
        p.seq = (uint32_t)i;
        p.tx_tick = (uint32_t)xTaskGetTickCount();
        sendto(s_fd, &p, sizeof(p), 0,
               (struct sockaddr *)&peer, sizeof(peer));
        g_sent++;
        vTaskDelay(pdMS_TO_TICKS(PKT_INTERVAL_MS));
    }
    vTaskDelete(NULL);
}

static void receiver_task(void *arg)
{
    burst_pkt_t p;
    int last_seq = -1;
    uint32_t last_arr = 0;
    (void)arg;

    while (g_rx_count < N_PKTS) {
        int n = recvfrom(s_fd, &p, sizeof(p), 0, NULL, NULL);
        if (n == (int)sizeof(p) && p.seq < N_PKTS) {
            uint32_t arr = (uint32_t)xTaskGetTickCount();
            if (!g_rx_seen[p.seq]) {
                g_rx_seen[p.seq] = 1;
                g_rx_arr[p.seq] = arr;
                g_rx_count++;
                if (last_seq >= 0 && (int)p.seq < last_seq)
                    g_ooo++;
                last_seq = (int)p.seq;
                if (last_arr != 0) {
                    int d = (int)(arr - last_arr);
                    if (g_min_int == 0 || d < g_min_int) g_min_int = d;
                    if (d > g_max_int) g_max_int = d;
                    g_sum_int += d;
                }
                last_arr = arr;
            }
        }
    }
    g_done = 1;
    vTaskDelete(NULL);
}

int net_burst_test_run(void)
{
    struct sockaddr_in local;
    int timeout = RX_TIMEOUT_MS;
    int w, loss, avg;

    printf("\r\n=== NET BURST TEST [%s]  %s:%u <-> %s:%u (200 pkts @10ms) ===\r\n",
           ROLE_NAME, MY_IP, MY_RX_PORT, PEER_IP, PEER_RX_PORT);

    s_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_fd < 0) {
        printf("netburst[%s]: socket failed\r\n", ROLE_NAME);
        return -1;
    }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons((u16_t)MY_RX_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        printf("netburst[%s]: bind failed\r\n", ROLE_NAME);
        return -1;
    }
    /* Give recvfrom() a timeout so the receiver can exit if <200 arrive. */
    setsockopt(s_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    /* READY handshake: both instances must start their 10ms burst at the
     * same wall-clock moment, otherwise the earlier instance sends all 200
     * packets before the later one starts listening (misread as "loss"). */
    {
        burst_pkt_t r;
        struct sockaddr_in peer;
        memset(&peer, 0, sizeof(peer));
        peer.sin_family = AF_INET;
        peer.sin_port = htons((u16_t)PEER_RX_PORT);
        peer.sin_addr.s_addr = inet_addr(PEER_IP);
        r.seq = READY_MAGIC;
        r.tx_tick = 0;
        sendto(s_fd, &r, sizeof(r), 0,
               (struct sockaddr *)&peer, sizeof(peer));
        {
            int sync_wait = 0;
            int n = recvfrom(s_fd, &r, sizeof(r), 0, NULL, NULL);
            while (n != (int)sizeof(r) || r.seq != READY_MAGIC) {
                if (n > 0 && r.seq != READY_MAGIC) {
                    /* stray packet - ignore */
                }
                vTaskDelay(pdMS_TO_TICKS(5));
                sync_wait += 5;
                if (sync_wait > 5000) {   /* peer never came up */ 
                    break;
                }
                n = recvfrom(s_fd, &r, sizeof(r), 0, NULL, NULL);
            }
        }
    }

    g_sent = g_rx_count = g_ooo = g_done = 0;
    g_min_int = g_max_int = 0;
    g_sum_int = 0;
    memset(g_rx_seen, 0, sizeof(g_rx_seen));
    memset(g_rx_arr, 0, sizeof(g_rx_arr));

    xTaskCreate(sender_task,   "nbt-tx", 2048, NULL, 2, NULL);
    xTaskCreate(receiver_task, "nbt-rx", 2048, NULL, 2, NULL);

    w = 0;
    while (!g_done && w < (RX_TIMEOUT_MS + 3000)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        w += 10;
    }
    vTaskDelay(pdMS_TO_TICKS(200));   /* drain tail */

    loss = N_PKTS - g_rx_count;
    avg = g_rx_count > 1 ? (int)(g_sum_int / (g_rx_count - 1)) : 0;
    printf("netburst[%s]: tx=%d rx=%d loss=%d(%d%%) ooo=%d "
           "interval(ms) min=%d avg=%d max=%d\r\n",
           ROLE_NAME, g_sent, g_rx_count, loss, loss * 100 / N_PKTS,
           g_ooo, g_min_int, avg, g_max_int);

    lwip_close(s_fd);
    s_fd = -1;
    return (loss == 0) ? 0 : -1;
}
