/**
 * @file pj_net_test.c
 * @brief PJLIB socket + ioqueue self-test over lwIP.
 *
 * Exercises:
 *   - pj_sock_socket / pj_sock_bind / pj_sock_sendto / pj_sock_recvfrom
 *   - pj_gethostip() (gets the board's interface IP)
 *   - pj_ioqueue_create / pj_ioqueue_register_sock / pj_ioqueue_poll
 *     / pj_ioqueue_recvfrom / pj_ioqueue_sendto (async path)
 *   - pj_sock_select() (select-based readiness check)
 *
 * A UDP datagram is sent from a client socket to the server socket on the
 * board's own IP; lwIP delivers it locally (packets to a local address are
 * looped to local sockets). The reply goes back the other way.
 */
#include <stdio.h>
#include <string.h>

#include "printf.h"
#include "pj_net_test.h"

#include <pj/os.h>
#include <pj/pool.h>
#include <pj/sock.h>
#include <pj/addr_resolv.h>
#include <pj/ioqueue.h>
#include <pj/sock_select.h>
#include <pj/errno.h>
#include <pj/string.h>

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            printf("pj_net: CHECK FAILED: %s (line %d)\r\n", \
                   #expr, __LINE__); \
            goto on_error; \
        } \
    } while (0)

#define SRV_PORT   15000

static pj_caching_pool g_cp;
static pj_ioqueue_t *g_ioq;

static pj_ioqueue_op_key_t g_rx_op;
static pj_ioqueue_op_key_t g_tx_op;
static char g_rxbuf[256];
static pj_ssize_t g_rx_len;
static pj_sockaddr g_rx_src;
static volatile int g_rx_done;

/* ------------------------------------------------------------------ */
/* ioqueue callbacks                                                   */
/* ------------------------------------------------------------------ */
static void on_read_complete(pj_ioqueue_key_t *key,
                             pj_ioqueue_op_key_t *op_key,
                             pj_ssize_t bytes_read)
{
    (void)key;
    (void)op_key;
    g_rx_len = bytes_read;
    g_rx_done = 1;
}

static void on_write_complete(pj_ioqueue_key_t *key,
                              pj_ioqueue_op_key_t *op_key,
                              pj_ssize_t bytes_sent)
{
    (void)key;
    (void)op_key;
    (void)bytes_sent;
}

static void on_accept_complete(pj_ioqueue_key_t *key,
                               pj_ioqueue_op_key_t *op_key,
                               pj_sock_t sock,
                               pj_status_t status)
{
    (void)key; (void)op_key; (void)sock; (void)status;
}

static void on_connect_complete(pj_ioqueue_key_t *key, pj_status_t status)
{
    (void)key; (void)status;
}

static pj_ioqueue_callback g_cb = {
    on_read_complete,
    on_write_complete,
    on_accept_complete,
    on_connect_complete
};

/* ------------------------------------------------------------------ */
/* ioqueue UDP test                                                    */
/* ------------------------------------------------------------------ */
static int test_ioqueue_udp(const pj_sockaddr_in *local_ip)
{
    pj_pool_t *pool = NULL;
    pj_ioqueue_t *ioq = NULL;
    pj_ioqueue_key_t *srv_key = NULL;
    pj_sock_t srv = PJ_INVALID_SOCKET, cli = PJ_INVALID_SOCKET;
    pj_sockaddr_in dst;
    pj_status_t rc;
    pj_ssize_t len;
    pj_time_val tmo;
    int i;

    pool = pj_pool_create(&g_cp.factory, "ioq", 8192, 1024, NULL);
    CHECK(pool != NULL);

    rc = pj_ioqueue_create(pool, 64, &ioq);
    CHECK(rc == PJ_SUCCESS);
    g_ioq = ioq;

    /* Server socket: bind to INADDR_ANY:SRV_PORT */
    CHECK(pj_sock_socket(PJ_AF_INET, PJ_SOCK_DGRAM, 0, &srv) == PJ_SUCCESS);
    pj_sockaddr_in_init(&dst, NULL, SRV_PORT);
    CHECK(pj_sock_bind(srv, &dst, sizeof(dst)) == PJ_SUCCESS);
    CHECK(pj_ioqueue_register_sock(pool, ioq, srv, NULL, &g_cb, &srv_key)
          == PJ_SUCCESS);

    /* Issue asynchronous recv. */
    g_rx_done = 0;
    g_rx_len = 0;
    {
        int addrlen = sizeof(g_rx_src);
        len = sizeof(g_rxbuf);
        rc = pj_ioqueue_recvfrom(srv_key, &g_rx_op, g_rxbuf, &len, 0,
                                 &g_rx_src, &addrlen);
        CHECK(rc == PJ_SUCCESS || rc == PJ_EPENDING);
    }

    /* Client socket: send "hello" to local_ip:SRV_PORT */
    CHECK(pj_sock_socket(PJ_AF_INET, PJ_SOCK_DGRAM, 0, &cli) == PJ_SUCCESS);
    pj_sockaddr_in_init(&dst, NULL, SRV_PORT);
    dst.sin_addr = local_ip->sin_addr;
    len = strlen("hello-pjlib");
    CHECK(pj_sock_sendto(cli, "hello-pjlib", &len, 0, &dst, sizeof(dst))
          == PJ_SUCCESS);

    /* Poll until the async recv completes. */
    tmo.sec = 2;
    tmo.msec = 0;
    for (i = 0; i < 20 && !g_rx_done; ++i) {
        int n = pj_ioqueue_poll(ioq, &tmo);
        if (n < 0)
            break;
    }
    CHECK(g_rx_done);
    CHECK(g_rx_len == (pj_ssize_t)strlen("hello-pjlib"));
    CHECK(memcmp(g_rxbuf, "hello-pjlib", (size_t)g_rx_len) == 0);
    printf("pj_net:   ioqueue recv OK: '%.*s' (%d bytes)\r\n",
           (int)g_rx_len, g_rxbuf, (int)g_rx_len);

    /* Reply "world" via ioqueue sendto, then client receives it. */
    {
        char rbuf[64];
        pj_ssize_t rlen = sizeof(rbuf);
        pj_fd_set_t rfds;
        pj_time_val stmo;

        len = strlen("world");
        CHECK(pj_ioqueue_sendto(srv_key, &g_tx_op, "world", &len, 0,
                                &g_rx_src, pj_sockaddr_get_len(&g_rx_src))
              == PJ_SUCCESS);

        /* select() readiness check before blocking recv. */
        PJ_FD_ZERO(&rfds);
        PJ_FD_SET(cli, &rfds);
        stmo.sec = 2;
        stmo.msec = 0;
        CHECK(pj_sock_select(cli + 1, &rfds, NULL, NULL, &stmo) > 0);
        CHECK(PJ_FD_ISSET(cli, &rfds));

        CHECK(pj_sock_recvfrom(cli, rbuf, &rlen, 0, NULL, NULL)
              == PJ_SUCCESS);
        CHECK(rlen == (pj_ssize_t)strlen("world"));
        CHECK(memcmp(rbuf, "world", (size_t)rlen) == 0);
        printf("pj_net:   ioqueue send/reply OK: '%.*s' (%d bytes)\r\n",
               (int)rlen, rbuf, (int)rlen);
    }

    pj_ioqueue_unregister(srv_key);
    pj_sock_close(srv);
    pj_sock_close(cli);
    pj_ioqueue_destroy(ioq);
    pj_pool_release(pool);
    return 0;

on_error:
    if (srv_key)
        pj_ioqueue_unregister(srv_key);
    if (srv != PJ_INVALID_SOCKET)
        pj_sock_close(srv);
    if (cli != PJ_INVALID_SOCKET)
        pj_sock_close(cli);
    if (ioq)
        pj_ioqueue_destroy(ioq);
    if (pool)
        pj_pool_release(pool);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Main entry                                                          */
/* ------------------------------------------------------------------ */
int pj_net_test_run(void)
{
    pj_sockaddr host;
    pj_sockaddr_in local_ip;
    pj_status_t rc;
    char ipstr[PJ_INET_ADDRSTRLEN];

    printf("\r\n=== PJLIB socket/ioqueue test (lwIP) ===\r\n");

    rc = pj_init();
    if (rc != PJ_SUCCESS) {
        printf("pj_net: pj_init() FAILED (%d)\r\n", rc);
        return -1;
    }
    pj_caching_pool_init(&g_cp, &pj_pool_factory_default_policy, 0);

    /* Get the board's interface IP (10.0.2.x via slirp). */
    rc = pj_gethostip(PJ_AF_INET, &host);
    if (rc != PJ_SUCCESS) {
        printf("pj_net: pj_gethostip() FAILED (%d)\r\n", rc);
        goto on_error;
    }
    local_ip = host.ipv4;
    printf("pj_net:   local IP = %s\r\n",
           pj_inet_ntop2(PJ_AF_INET, &local_ip.sin_addr, ipstr,
                         sizeof(ipstr)));

    if (test_ioqueue_udp(&local_ip) != 0)
        goto on_error;

    printf("pj_net: ALL PASSED\r\n");
    pj_shutdown();
    return 0;

on_error:
    printf("pj_net: FAILED\r\n");
    pj_shutdown();
    return -1;
}
