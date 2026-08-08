/**
 * @file    lan9118_netif.c
 * @brief   lwIP netif glue for the LAN9118 driver.
 *
 * Compiled only when LAN9118_HAVE_LWIP is defined (FreeRTOS project).
 * The file is intentionally guarded so other projects (BareMetal /
 * ThreadX) can keep it in the source tree as an empty translation unit.
 */
#ifdef LAN9118_HAVE_LWIP

#include <string.h>

#include "lan9118.h"
#include "lan9118_osal.h"
#include "lan9118_netif.h"

#include "lwip/opt.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "lwip/prot/ethernet.h"
#include "netif/ethernet.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"

#ifndef LAN9118_NETIF_MAX_RX
#define LAN9118_NETIF_MAX_RX     (16U)
#endif
#define LAN9118_NETIF_POLL_MS    (500U)

static err_t lan9118_low_level_output(struct netif *netif, struct pbuf *p);

static void lan9118_netif_link_cb(bool up, void *arg) {
    struct netif *netif = (struct netif *)arg;
#if !NO_SYS
    /* netif_set_link_up/down are lwIP core functions: in OS mode they must
       be called from the tcpip_thread context or while holding the core lock
       (the link callback runs from the lan9118_netif_thread task). */
    LOCK_TCPIP_CORE();
#endif
    if (up) {
        netif_set_link_up(netif);
    } else {
        netif_set_link_down(netif);
    }
#if !NO_SYS
    UNLOCK_TCPIP_CORE();
#endif
}

static err_t lan9118_low_level_init(struct netif *netif) {
    uint8_t mac[6] = { 0 };
    lan9118_get_mac(mac);
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, mac, ETH_HWADDR_LEN);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
#if NO_SYS
    netif->input = ethernet_input;
#else
    netif->input = tcpip_input;
#endif
#if LWIP_IPV4 && LWIP_ARP
    netif->output = etharp_output;
#endif
#if LWIP_IPV6
    netif->output_ip6 = ethip6_output;
#endif
    /* Low-level Ethernet frame transmit hook (LAN9118 TX path). */
    netif->linkoutput = lan9118_low_level_output;
    /* Report the current link state and register the change callback. */
    lan9118_set_link_callback(lan9118_netif_link_cb, netif);
    lan9118_link_poll();
    if (lan9118_link_status()) {
        netif_set_link_up(netif);
    } else {
        netif_set_link_down(netif);
    }
    return ERR_OK;
}

err_t lan9118_netif_init(struct netif *netif) {
    return lan9118_low_level_init(netif);
}

static err_t lan9118_low_level_output(struct netif *netif, struct pbuf *p) {
    static uint8_t buf[LAN9118_MAX_FRAME];
    uint16_t len = 0;
    struct pbuf *q;
    (void)netif;

    if (p->tot_len > (u16_t)sizeof(buf)) {
        return ERR_MEM;
    }
    /* Linearize the pbuf chain into a contiguous frame buffer. */
    for (q = p; q != NULL; q = q->next) {
        memcpy(&buf[len], q->payload, q->len);
        len = (u16_t)(len + q->len);
    }
    if (lan9118_send(buf, len) != LAN9118_OK) {
        return ERR_IF;
    }
    return ERR_OK;
}

err_t lan9118_netif_output(struct netif *netif, struct pbuf *p) {
    return lan9118_low_level_output(netif, p);
}

static struct pbuf *lan9118_low_level_input(struct netif *netif) {
    struct pbuf *p = NULL;
    uint32_t got = 0;
    int plen = 0;
    (void)netif;

    plen = lan9118_peek_frame_len();
    if (plen <= 0) {
        return NULL;
    }
    /* the MAC includes the 4-byte FCS */
    plen -= LAN9118_FCS_LEN;
    p = pbuf_alloc(PBUF_RAW, (u16_t)plen, PBUF_RAM);
    if (p == NULL) {
        /* No pbuf available: drop the frame to keep the FIFO draining. */
        uint8_t tmp[128] = { 0 };
        uint32_t l;
        (void)lan9118_read_frame(tmp, sizeof(tmp), &l);
        return NULL;
    }
    if (lan9118_read_frame((uint8_t *)p->payload, (uint32_t)plen, &got) !=
        LAN9118_OK) {
        pbuf_free(p);
        return NULL;
    }
    p->len = (u16_t)got;
    p->tot_len = p->len;
    return p;
}

void lan9118_netif_input(struct netif *netif) {
    int budget = (int)LAN9118_NETIF_MAX_RX;
    while ((budget-- > 0) && lan9118_rx_pending()) {
        struct pbuf *p = lan9118_low_level_input(netif);
        if (p == NULL) {
            break;
        }
        if (netif->input(p, netif) != ERR_OK) {
            pbuf_free(p);
        }
    }
}

void lan9118_netif_isr(struct netif *netif) {
    (void)netif;
    /* clears IRQ status and signals the RX semaphore */
    lan9118_isr();
}

void lan9118_netif_poll(struct netif *netif) {
    lan9118_link_poll();
    lan9118_netif_input(netif);
}

#if !NO_SYS
void lan9118_netif_thread(void *arg) {
    struct netif *netif = (struct netif *)arg;
    while (1) {
        if (lan9118_osal_sem_take(LAN9118_NETIF_POLL_MS)) {
            lan9118_netif_input(netif);
        }
        lan9118_link_poll();
    }
}
#else
void lan9118_netif_thread(void *arg) {
    (void)arg;
}
#endif

#endif /* LAN9118_HAVE_LWIP */
