#ifndef LAN9118_H
#define LAN9118_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "lan9118_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* max Ethernet frame (incl FCS) */
#define LAN9118_MAX_FRAME                   (1518U)
#define LAN9118_ETH_HDR_LEN                 (14U)
#define LAN9118_FCS_LEN                     (4U)

typedef enum {
    LAN9118_OK = 0,
    /* bad argument */
    LAN9118_ERR_PARAM = -1,
    /* hardware not present / probe failed */
    LAN9118_ERR_IO = -2,
    /* internal timeout (CSR/PHY/FIFO) */
    LAN9118_ERR_TIMEOUT = -3,
    /* resource busy / TX in progress */
    LAN9118_ERR_BUSY = -4,
    /* driver not opened */
    LAN9118_ERR_NOT_READY = -5,
    /* no RX packet available */
    LAN9118_ERR_NO_PKT = -6,
    /* link is down */
    LAN9118_ERR_LINK_DOWN = -7,
} lan9118_err_t;

typedef struct {
    /* all-zero: derive from EEPROM, then fallback */
    uint8_t mac[6];
    /* accept all frames */
    bool promiscuous;
    /* pass all multicast (MCPAS) */
    bool multicast;
    /* default true; set false to reject bcast */
    bool accept_broadcast;
    /* deliver frames that failed the filter */
    bool pass_bad_frames;
    /* MAC internal loopback */
    bool loopback;    
    /* enable RX/TX pause (AFC + LAN9118_MAC_FLOW) */      
    bool flow_control;
    /* RXDOFF 0..31 */
    uint8_t rx_offset;
    /* enable NVIC IRQ + LAN9118 interrupts */
    bool enable_irq;
    /* NVIC priority when enable_irq is set */
    uint8_t irq_prio;
    /* TX FIFO / TX-completion budget */       
    uint32_t tx_timeout_ms;
    /* recommended link poll period (advisory) */     
    uint32_t link_poll_ms;
} lan9118_config_t;

/* == configMAX_SYSCALL_INTERRUPT_PRIORITY */
#define LAN9118_DEFAULT_IRQ_PRIO            (191U)
#define LAN9118_DEFAULT_TX_TIMEOUT_MS       (100U)
#define LAN9118_DEFAULT_LINK_POLL_MS        (500U)

typedef struct {
    /* frames delivered to the consumer */
    uint32_t rx_packets;
    uint32_t rx_bytes;
    /* frames with error flags */
    uint32_t rx_errors;
    uint32_t rx_crc_errors;
    uint32_t rx_frame_errors;
    /* RX FIFO under/over-run events */
    uint32_t rx_overruns;
    uint32_t rx_broadcast;
    uint32_t rx_multicast;
    /* dropped (consumer not ready / no memory) */
    uint32_t rx_dropped;
    /* frames written to the MAC */
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t tx_errors;
    uint32_t tx_collisions;
    uint32_t tx_timeouts;
    uint32_t tx_fifo_overruns;
    uint32_t link_ups;
    uint32_t link_downs;
    uint32_t irq_count;
} lan9118_stats_t;

/* Called by lan9118_rx_process() for every delivered frame. */
typedef void (*lan9118_rx_cb_t)(const uint8_t *data, uint32_t len, void *arg);
/* Called on link state change (from lan9118_link_poll / ISR). */
typedef void (*lan9118_link_cb_t)(bool up, void *arg);
/* NULL = defaults */
int lan9118_init(const lan9118_config_t *cfg);
/* init with defaults */
int lan9118_open(void);
void lan9118_close(void);
bool lan9118_is_open(void);

/* MAC address */
int lan9118_set_mac(const uint8_t mac[6]);
void lan9118_get_mac(uint8_t mac[6]);

/* Link 1 = up, 0 = down */
int lan9118_link_status(void);
/* poll PHY; fires link callback on change */
void lan9118_link_poll(void);

/* Transmit a raw Ethernet frame (blocking, task context). */
int lan9118_send(const uint8_t *data, uint32_t len);

/* Receive - non-blocking pull model, stack agnostic. */
/* frames in RX FIFO */
int lan9118_rx_pending(void);
/* next frame length */
int lan9118_peek_frame_len(void);
int lan9118_read_frame(uint8_t *buf, uint32_t buf_size, uint32_t *len);
/* pump -> rx callback */
void lan9118_rx_process(void);

/* Callbacks */
void lan9118_set_rx_callback(lan9118_rx_cb_t cb, void *arg);
void lan9118_set_link_callback(lan9118_link_cb_t cb, void *arg);

/* Interrupt service routine - call from the LAN9118 vector (IRQ 48). */
void lan9118_isr(void);

/* Statistics */
const lan9118_stats_t *lan9118_get_stats(void);
void lan9118_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* LAN9118_H */
