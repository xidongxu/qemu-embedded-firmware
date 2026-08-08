/**
 * @file    lan9118_netif.h
 * @brief   lwIP netif adapter for the LAN9118 driver (lan9118.c).
 *
 * This is the concrete example of how the stack-agnostic lan9118 driver
 * is plugged into lwIP.  It follows the canonical ethernetif pattern
 * (low_level_init / low_level_output / low_level_input).
 *
 * Usage:
 *   - NO_SYS (polled):
 *       struct netif nif;
 *       netif_add(&nif, &ip, &mask, &gw, NULL, lan9118_netif_init,
 *                 ethernet_input);
 *       netif_set_default(&nif);
 *       for (;;) { lan9118_netif_poll(&nif); sys_check_timeouts(); }
 *   - With an RTOS (sys_arch + tcpip thread):
 *       netif_add(&nif, ..., lan9118_netif_init, tcpip_input);
 *       xTaskCreate(lan9118_netif_thread, "eth", ... , &nif, ...);
 *       // ISR (IRQ 48) must call lan9118_netif_isr(&nif);
 */
#ifndef LAN9118_NETIF_H
#define LAN9118_NETIF_H

#include "lwip/opt.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* low_level_init */
err_t lan9118_netif_init(struct netif *netif);
err_t lan9118_netif_output(struct netif *netif, struct pbuf *p);
/* drain RX into lwIP */
void lan9118_netif_input(struct netif *netif);
/* call from the ISR */
void lan9118_netif_isr(struct netif *netif);
/* NO_SYS main-loop helper */
void lan9118_netif_poll(struct netif *netif);
/* RTOS worker thread body */
void lan9118_netif_thread(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* LAN9118_NETIF_H */
