/**
 * @file    lan9118.c
 * @brief   SMSC LAN9118 (LAN9220-compatible) Ethernet MAC driver.
 *
 * Industrial-grade, protocol-stack agnostic implementation:
 *   - complete power-on / reset / probe sequence
 *   - MAC + PHY bring-up with auto-negotiation and link monitoring
 *   - interrupt-driven RX (deferred to the stack via the OSAL semaphore)
 *     and a non-blocking pull API for bare-metal / NO_SYS operation
 *   - robust TX with FIFO space management, completion tracking and
 *     status-FIFO error handling
 *   - MAC address management (config -> EEPROM -> fallback)
 *   - per-interface statistics
 *   - no RTOS / TCP-IP stack dependency: everything goes through the
 *     OSAL (lan9118_osal.h) and callbacks.
 *
 * The lwIP adapter lives in lan9118_netif.c; other stacks only need to
 * consume lan9118_send() / lan9118_read_frame() + the callbacks.
 */
#include <string.h>

#include "lan9118.h"
#include "lan9118_osal.h"
#include "printf.h"

#ifndef LAN9118_CSR_TIMEOUT_MS
#define LAN9118_CSR_TIMEOUT_MS          (10U)
#endif
#ifndef LAN9118_PHY_AUTONEG_TIMEOUT_MS
#define LAN9118_PHY_AUTONEG_TIMEOUT_MS  (1000U)
#endif
#ifndef LAN9118_RX_MAX_FRAMES
#define LAN9118_RX_MAX_FRAMES           (16U)
#endif
#ifndef LAN9118_SOFTWARE_LOOPBACK
#define LAN9118_SOFTWARE_LOOPBACK       (0)
#endif
/* Debug prints (off by default in production) */
#ifndef LAN9118_DEBUG
#define LAN9118_DEBUG                   (0)
#endif
#if LAN9118_DEBUG
#define lan_dbg(fmt, ...)               printf("[lan9118] " fmt "\n", ##__VA_ARGS__)
#else
#define lan_dbg(fmt, ...)               do { } while (0)
#endif

static inline uint32_t lan_read(uint32_t reg) {
    return *(volatile uint32_t *)(LAN9118_BASE + reg);
}

static inline void lan_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(LAN9118_BASE + reg) = value;
}

typedef struct {
    bool opened;
    lan9118_config_t cfg;
    lan9118_stats_t stats;
    lan9118_rx_cb_t rx_cb;
    void *rx_cb_arg;
    lan9118_link_cb_t link_cb;
    void *link_cb_arg;
    uint8_t mac[6];
    bool link_up;
    volatile bool tx_busy;
#if LAN9118_SOFTWARE_LOOPBACK
    uint8_t lb_frame[LAN9118_MAX_FRAME];
    uint32_t lb_len;
    bool lb_pending;
#endif
} lan9118_priv_t;

static lan9118_priv_t s_p = { 0 };

static int lan9118_poll_bits(uint32_t reg, uint32_t mask, int want, uint32_t timeout_ms) {
    uint32_t start = lan9118_osal_time_ms();
    while (!!(lan_read(reg) & mask) != want) {
        if ((lan9118_osal_time_ms() - start) >= timeout_ms) {
            return LAN9118_ERR_TIMEOUT;
        }
    }
    return LAN9118_OK;
}

static int lan9118_mac_read(uint8_t reg, uint32_t *val) {
    int ret = 0;
    if (val == NULL) {
        return LAN9118_ERR_PARAM;
    }
    ret = lan9118_poll_bits(LAN9118_MAC_CSR_CMD, LAN9118_MAC_CSR_BUSY, 0,
                            LAN9118_CSR_TIMEOUT_MS);
    if (ret != LAN9118_OK) {
        return ret;
    }
    lan_write(LAN9118_MAC_CSR_CMD,
              LAN9118_MAC_CSR_BUSY | \
              LAN9118_MAC_CSR_READ | \
              LAN9118_MAC_CSR_ADDR(reg));
    ret = lan9118_poll_bits(LAN9118_MAC_CSR_CMD, LAN9118_MAC_CSR_BUSY, 0, LAN9118_CSR_TIMEOUT_MS);
    if (ret != LAN9118_OK) {
        return ret;
    }
    *val = lan_read(LAN9118_MAC_CSR_DATA);
    return LAN9118_OK;
}

static int lan9118_mac_write(uint8_t reg, uint32_t val) {
    int ret = 0;
    ret = lan9118_poll_bits(LAN9118_MAC_CSR_CMD, LAN9118_MAC_CSR_BUSY, 0, LAN9118_CSR_TIMEOUT_MS);
    if (ret != LAN9118_OK) {
        return ret;
    }
    lan_write(LAN9118_MAC_CSR_DATA, val);
    lan_write(LAN9118_MAC_CSR_CMD, LAN9118_MAC_CSR_BUSY | LAN9118_MAC_CSR_ADDR(reg));
    return lan9118_poll_bits(LAN9118_MAC_CSR_CMD, LAN9118_MAC_CSR_BUSY, 0, LAN9118_CSR_TIMEOUT_MS);
}

/* The MII interface is reached through the MAC CSR space; poll the
 * MII_ACC busy bit by reading it over the MAC CSR interface. */
static int lan9118_phy_wait_idle(uint32_t timeout_ms) {
    uint32_t start = lan9118_osal_time_ms();
    while(true) {
        uint32_t acc = 0;
        int ret = lan9118_mac_read(LAN9118_MAC_MII_ACC, &acc);
        if (ret != LAN9118_OK) {
            return ret;
        }
        if ((acc & LAN9118_MII_ACC_BUSY) == 0U) {
            return LAN9118_OK;
        }
        if ((lan9118_osal_time_ms() - start) >= timeout_ms) {
            return LAN9118_ERR_TIMEOUT;
        }
    }
}

static int lan9118_phy_read(uint8_t reg, uint16_t *val) {
    uint32_t tmp = 0;
    int ret = 0;
    if (val == NULL) {
        return LAN9118_ERR_PARAM;
    }
    ret = lan9118_phy_wait_idle(LAN9118_CSR_TIMEOUT_MS);
    if (ret != LAN9118_OK) {
        return ret;
    }
    ret = lan9118_mac_write(LAN9118_MAC_MII_ACC,
                            (uint32_t)(LAN9118_PHY_ADDR << LAN9118_MII_PHY_ADDR_SHIFT) |
                            (uint32_t)(reg << LAN9118_MII_REG_ADDR_SHIFT));
    if (ret != LAN9118_OK) {
        return ret;
    }
    ret = lan9118_phy_wait_idle(LAN9118_CSR_TIMEOUT_MS);
    if (ret != LAN9118_OK) {
        return ret;
    }
    ret = lan9118_mac_read(LAN9118_MAC_MII_DATA, &tmp);
    if (ret != LAN9118_OK) {
        return ret;
    }
    *val = (uint16_t)(tmp & 0xffffU);
    return LAN9118_OK;
}

static int lan9118_phy_write(uint8_t reg, uint16_t val) {
    int ret = 0;
    ret = lan9118_phy_wait_idle(LAN9118_CSR_TIMEOUT_MS);
    if (ret != LAN9118_OK) {
        return ret;
    }
    ret = lan9118_mac_write(LAN9118_MAC_MII_DATA, val);
    if (ret != LAN9118_OK) {
        return ret;
    }
    ret = lan9118_mac_write(LAN9118_MAC_MII_ACC,
                            LAN9118_MII_ACC_WRITE |
                            (uint32_t)(LAN9118_PHY_ADDR << LAN9118_MII_PHY_ADDR_SHIFT) |
                            (uint32_t)(reg << LAN9118_MII_REG_ADDR_SHIFT));
    if (ret != LAN9118_OK) {
        return ret;
    }
    return lan9118_phy_wait_idle(LAN9118_CSR_TIMEOUT_MS);
}

static uint8_t lan9118_eeprom_read(uint8_t addr) {
    uint32_t start = 0;
    (void)lan9118_poll_bits(LAN9118_E2P_CMD, LAN9118_E2P_CMD_E2P_BUSY, 0, LAN9118_CSR_TIMEOUT_MS);
    lan_write(LAN9118_E2P_CMD, LAN9118_E2P_CMD_READ | LAN9118_E2P_CMD_ADDR(addr));
    start = lan9118_osal_time_ms();
    while ((lan_read(LAN9118_E2P_CMD) & LAN9118_E2P_CMD_E2P_BUSY) != 0U) {
        if ((lan9118_osal_time_ms() - start) >= LAN9118_CSR_TIMEOUT_MS) {
            break;
        }
    }
    return (uint8_t)(lan_read(LAN9118_E2P_DATA) & 0xffU);
}

static bool lan9118_mac_from_eeprom(uint8_t mac[6]) {
    int i = 0;
    /* signature check */
    if (lan9118_eeprom_read(0) != 0xa5U) {
        return false;
    }
    for (i = 0; i < 6; i++) {
        mac[i] = lan9118_eeprom_read((uint8_t)(1 + i));
    }
    return true;
}

static int lan9118_hw_reset(void) {
    lan_write(LAN9118_HW_CFG, lan_read(LAN9118_HW_CFG) | LAN9118_HW_CFG_SRST);
    return lan9118_poll_bits(LAN9118_HW_CFG, LAN9118_HW_CFG_SRST, 0, LAN9118_CSR_TIMEOUT_MS);
}

static int lan9118_probe(void) {
    uint32_t id  = lan_read(LAN9118_ID_REV);
    uint32_t btr = lan_read(LAN9118_BYTE_TEST);
    if (id == 0U || id == 0xFFFFFFFFU) {
        return LAN9118_ERR_IO;
    }
    if (btr != LAN9118_BYTE_TEST_VALUE) {
        return LAN9118_ERR_IO;
    }
    if ((id >> 16) != (LAN9118_ID_REV_VALUE >> 16)) {
        return LAN9118_ERR_IO;
    }
    lan_dbg("ID=0x%08lx BYTE_TEST=0x%08lx", (unsigned long)id, (unsigned long)btr);
    return LAN9118_OK;
}

static int lan9118_mac_set_addr(const uint8_t mac[6]) {
    uint32_t lo = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                  ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    uint32_t hi = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);
    int ret = 0;
    ret = lan9118_mac_write(LAN9118_MAC_ADDRL, lo);
    if (ret != LAN9118_OK) {
        return ret;
    }
    return lan9118_mac_write(LAN9118_MAC_ADDRH, hi);
}

/* Keep LAN9118_MAC_CR in sync with the negotiated duplex / config. */
static void lan9118_mac_configure(void) {
    uint32_t cr = LAN9118_MAC_CR_TXEN | LAN9118_MAC_CR_RXEN;
    bool fd = true;
    if (s_p.cfg.loopback) {
        cr |= LAN9118_MAC_CR_LOOPBK;
    }
    if (s_p.cfg.promiscuous) {
        cr |= LAN9118_MAC_CR_PRMS;
    }
    if (s_p.cfg.multicast) {
        cr |= LAN9118_MAC_CR_MCPAS;
    }
    if (!s_p.cfg.accept_broadcast) {
        /* set = reject broadcast */
        cr |= LAN9118_MAC_CR_BCAST;
    }
    if (s_p.cfg.pass_bad_frames) {
        cr |= LAN9118_MAC_CR_PASSBAD;
    }
    /* Derive duplex from the PHY when the link is up. */
    if (s_p.link_up) {
        uint16_t anlpar = 0, bcr = 0;
        if (lan9118_phy_read(LAN9118_PHY_ANLPAR, &anlpar) == LAN9118_OK &&
            lan9118_phy_read(LAN9118_PHY_BCR, &bcr) == LAN9118_OK) {
            if ((anlpar & LAN9118_PHY_ANLPAR_TXFD) != 0U) {
                /* 100Base-TX full duplex */
                fd = true;
            } else if ((anlpar & LAN9118_PHY_ANLPAR_10FD) != 0U) {
                /* 10Base-T full duplex */
                fd = true;
            } else if ((anlpar & (LAN9118_PHY_ANLPAR_TX | LAN9118_PHY_ANLPAR_10)) != 0U) {
                /* half duplex */
                fd = false;
            } else if ((bcr & LAN9118_PHY_BCR_FULL_DUPLEX) != 0U) {
                fd = true;
            }
        }
    }
    if (fd) {
        cr |= LAN9118_MAC_CR_FDPX;
    }
    (void)lan9118_mac_write(LAN9118_MAC_CR, cr);
    lan_dbg("LAN9118_MAC_CR=0x%08lx", (unsigned long)cr);
}

static bool lan9118_phy_link_up(void) {
    uint16_t bsr = 0;
    if (lan9118_phy_read(LAN9118_PHY_BSR, &bsr) != LAN9118_OK) {
        return false;
    }
    return (bsr & LAN9118_PHY_BSR_LINK_UP) != 0U;
}

static void lan9118_phy_wait_autoneg(uint32_t timeout_ms) {
    uint32_t start = lan9118_osal_time_ms();
    uint16_t bsr = 0;
    do {
        if (lan9118_phy_read(LAN9118_PHY_BSR, &bsr) != LAN9118_OK) {
            return;
        }
        if ((bsr & LAN9118_PHY_BSR_AUTO_NEG_COMP) != 0U) {
            return;
        }
    } while ((lan9118_osal_time_ms() - start) < timeout_ms);
}

static int lan9118_phy_setup(void) {
    uint16_t id1 = 0, id2 = 0;
    int ret = 0;
    ret = lan9118_phy_read(LAN9118_PHY_ID1, &id1);
    if (ret != LAN9118_OK) {
        return ret;
    }
    ret = lan9118_phy_read(LAN9118_PHY_ID2, &id2);
    if (ret != LAN9118_OK) {
        return ret;
    }
    lan_dbg("PHY ID1=0x%04x ID2=0x%04x", id1, id2);
    /* PHY soft reset */
    ret = lan9118_phy_write(LAN9118_PHY_BCR, LAN9118_PHY_BCR_RESET);
    if (ret != LAN9118_OK) {
        return ret;
    }
    lan9118_osal_delay_ms(2);
    /* Advertise 100Base-TX and 10Base-T (FD + HD), enable auto-negotiation */
    ret = lan9118_phy_write(LAN9118_PHY_BCR, 
                            LAN9118_PHY_BCR_AUTO_NEG | \
                            LAN9118_PHY_BCR_AN_RESTART | \
                            LAN9118_PHY_BCR_SPEED100 | \
                            LAN9118_PHY_BCR_FULL_DUPLEX);
    if (ret != LAN9118_OK) {
        return ret;
    }
    /* Wait for negotiation; timeouts are tolerated (some PHYs / models
     * complete instantly or never report completion). */
    lan9118_phy_wait_autoneg(LAN9118_PHY_AUTONEG_TIMEOUT_MS);
    return LAN9118_OK;
}

static void lan9118_rx_configure(void) {
    lan_write(LAN9118_RX_CFG,
              LAN9118_RX_CFG_RXDOFF(s_p.cfg.rx_offset & 0x1fU) | \
              LAN9118_RX_CFG_RX_DMA_CNT(0) | \
              LAN9118_RX_CFG_RX_END_ALGN4);
}

static void lan9118_afc_configure(void) {
    if (s_p.cfg.flow_control) {
        lan_write(LAN9118_AFC_CFG,
                  LAN9118_AFC_CFG_VALID |
                  LAN9118_AFC_CFG_HI_LEVEL(0x0fU) |
                  LAN9118_AFC_CFG_LO_LEVEL(0x05U) |
                  LAN9118_AFC_CFG_BACK_DUR(0x07U));
        (void)lan9118_mac_write(LAN9118_MAC_FLOW,
                                LAN9118_MAC_FLOW_FCPT(0xffffU) |
                                LAN9118_MAC_FLOW_FCPI(0x60U) |
                                LAN9118_MAC_FLOW_RX_FLOW | \
                                LAN9118_MAC_FLOW_TX_FLOW);
    } else {
        lan_write(LAN9118_AFC_CFG, 0U);
        (void)lan9118_mac_write(LAN9118_MAC_FLOW, 0U);
    }
}

static void lan9118_irq_setup(void) {
    /* Clear any stale status and disable everything first. */
    lan_write(LAN9118_INT_EN, 0U);
    lan_write(LAN9118_INT_STS, 0xFFFFFFFFU);

    if (!s_p.cfg.enable_irq) {
        lan9118_osal_irq_disable();
        return;
    }

    /* Active-high, push-pull interrupt line. */
    lan_write(LAN9118_IRQ_CFG, 
              LAN9118_IRQ_CFG_IRQ_EN | \
              LAN9118_IRQ_CFG_IRQ_POL |
              LAN9118_IRQ_CFG_IRQ_TYPE);

    lan_write(LAN9118_INT_EN,
              LAN9118_INT_RX_EVENTS | \
              LAN9118_INT_TX_EVENTS | \
              LAN9118_INT_PHY_INT | \
              LAN9118_INT_FIFO_ERR);

    lan9118_osal_irq_priority(s_p.cfg.irq_prio);
    lan9118_osal_irq_enable();
}

static void lan9118_tx_status_process(void) {
    uint32_t inf = 0;

    while (1) {
        inf = lan_read(LAN9118_TX_FIFO_INF);
        if (((inf >> 16) & 0xffffU) == 0U) {
            /* no more status entries */
            break;
        }
        {
            uint32_t sts = lan_read(LAN9118_TX_STATUS_FIFO);
            if (sts & (LAN9118_TX_STS_EXCESS_COLL | \
                       LAN9118_TX_STS_LATE_COLL |
                       LAN9118_TX_STS_EXCESS_DEF | \
                       LAN9118_TX_STS_NO_CARRIER | \
                       LAN9118_TX_STS_LOSS_CARRIER | \
                       LAN9118_TX_STS_TX_ERR)) {
                s_p.stats.tx_errors++;
                if (sts & LAN9118_TX_STS_EXCESS_COLL) {
                    s_p.stats.tx_collisions++;
                }
            }
        }
        /* MAC consumed the previous packet */
        s_p.tx_busy = false;
    }
}

/* Wait for the previous TX packet to be consumed by the MAC.  Works in
 * both interrupt-driven and polled modes. */
static void lan9118_tx_wait_complete(uint32_t timeout_ms) {
    uint32_t start = lan9118_osal_time_ms();
    while (s_p.tx_busy) {
        if (((lan_read(LAN9118_TX_FIFO_INF) >> 16) & 0xffffU) != 0U) {
            lan9118_tx_status_process();
        }
        if (!s_p.tx_busy) {
            return;
        }
        if ((lan9118_osal_time_ms() - start) >= timeout_ms) {
            return;
        }
    }
}

int lan9118_send(const uint8_t *data, uint32_t len) {
    uint32_t words, free_units = 0, i, start;
    if (!s_p.opened) {
        return LAN9118_ERR_NOT_READY;
    }
    if (data == NULL || len < LAN9118_ETH_HDR_LEN) {
        return LAN9118_ERR_PARAM;
    }
    if (len > LAN9118_MAX_FRAME) {
        return LAN9118_ERR_PARAM;
    }
    /* LAN9118 is a single-packet MAC: the previous packet must have been
     * consumed before a new TX command word may be written. */
    lan9118_tx_wait_complete(s_p.cfg.tx_timeout_ms);
    if (s_p.tx_busy) {
        s_p.stats.tx_timeouts++;
        return LAN9118_ERR_TIMEOUT;
    }
    words = (len + 3U) / 4U;
    lan9118_osal_lock();
    /* Wait for enough free space: 2 command words + payload words. */
    start = lan9118_osal_time_ms();
    do {
        free_units = LAN9118_TX_FIFO_INF_FREE(lan_read(LAN9118_TX_FIFO_INF));
        if (free_units >= (words + 2U)) {
            break;
        }
    } while ((lan9118_osal_time_ms() - start) < s_p.cfg.tx_timeout_ms);
    if (free_units < (words + 2U)) {
        s_p.stats.tx_fifo_overruns++;
        lan9118_osal_unlock();
        return LAN9118_ERR_TIMEOUT;
    }
    /* TX command A: first+last segment, interrupt-on-completion. */
    lan_write(LAN9118_TX_DATA_FIFO, 
              LAN9118_TX_CMD_A_FIRST_SEG | \
              LAN9118_TX_CMD_A_LAST_SEG | \
              LAN9118_TX_CMD_A_IOC | \
              LAN9118_TX_CMD_A_BUF_SIZE(len));
    /* TX command B: data size (high) + buffer size (low). */
    lan_write(LAN9118_TX_DATA_FIFO,
              LAN9118_TX_CMD_B_DATA_SIZE(len) | \
              LAN9118_TX_CMD_B_BUF_SIZE(len));
    /* Payload, native little-endian words; the final partial word is
     * zero padded (padding bytes are ignored by the MAC). */
    for (i = 0; i < words; i++) {
        uint32_t v = 0;
        uint32_t off = i * 4U;
        uint32_t n = len - off;
        if (n > 4U) {
            n = 4U;
        }
        memcpy(&v, &data[off], n);
        lan_write(LAN9118_TX_DATA_FIFO, v);
    }
    s_p.tx_busy = true;
    lan9118_osal_unlock();
    s_p.stats.tx_packets++;
    s_p.stats.tx_bytes += len;

#if LAN9118_SOFTWARE_LOOPBACK
    if (len <= sizeof(s_p.lb_frame)) {
        memcpy(s_p.lb_frame, data, len);
        s_p.lb_len = len;
        s_p.lb_pending = true;
    }
#endif
    return LAN9118_OK;
}

int lan9118_rx_pending(void) {
    if (!s_p.opened) {
        return 0;
    }
    return (int)LAN9118_RX_FIFO_INF_PKT_CNT(lan_read(LAN9118_RX_FIFO_INF));
}

int lan9118_peek_frame_len(void) {
    uint32_t sts = 0;
    if (!s_p.opened || lan9118_rx_pending() == 0) {
        return 0;
    }
    sts = lan_read(LAN9118_RX_STATUS_PEEK);
    if (sts == 0U || sts == 0xFFFFFFFFU) {
        return 0;
    }
    /* includes FCS */
    return (int)LAN9118_RX_STS_PKT_LEN(sts);
}

int lan9118_read_frame(uint8_t *buf, uint32_t buf_size, uint32_t *len) {
    uint32_t sts = 0, pkt_len = 0, frame_len = 0, words = 0, i = 0;
    if (!s_p.opened) {
        return LAN9118_ERR_NOT_READY;
    }
    if (buf == NULL || len == NULL) {
        return LAN9118_ERR_PARAM;
    }
#if LAN9118_SOFTWARE_LOOPBACK
    if (s_p.lb_pending) {
        frame_len = s_p.lb_len;
        if (frame_len > buf_size) {
            frame_len = buf_size;
        }
        memcpy(buf, s_p.lb_frame, frame_len);
        *len = frame_len;
        s_p.lb_pending = false;
        return LAN9118_OK;
    }
#endif
    if (lan9118_rx_pending() == 0) {
        *len = 0;
        return LAN9118_ERR_NO_PKT;
    }
    /* Pop the RX status word. */
    sts = lan_read(LAN9118_RX_STATUS_FIFO);
    /* includes 4-byte FCS */
    pkt_len = LAN9118_RX_STS_PKT_LEN(sts);

    if (pkt_len < (LAN9118_ETH_HDR_LEN + LAN9118_FCS_LEN) || \
        pkt_len > (LAN9118_MAX_FRAME + LAN9118_FCS_LEN)) {
        /* Invalid frame: drain it to keep the FIFO aligned. */
        words = (pkt_len + 3U) / 4U;
        for (i = 0; i < words; i++) {
            (void)lan_read(LAN9118_RX_DATA_FIFO);
        }
        s_p.stats.rx_errors++;
        return LAN9118_ERR_IO;
    }
    /* strip FCS */
    frame_len = pkt_len - LAN9118_FCS_LEN;
    if (frame_len > buf_size) {
        /* truncate safely */
        frame_len = buf_size;
    }
    /* Read the payload (whole words, FCS word included to keep the
     * FIFO aligned). */
    words = (pkt_len + 3U) / 4U;
    for (i = 0; i < words; i++) {
        uint32_t v = lan_read(LAN9118_RX_DATA_FIFO);
        uint32_t off = i * 4U;
        if (off < frame_len) {
            uint32_t n = frame_len - off;
            if (n > 4U) {
                n = 4U;
            }
            memcpy(&buf[off], &v, n);
        }
    }
    *len = frame_len;
    s_p.stats.rx_packets++;
    s_p.stats.rx_bytes += frame_len;
    if (sts & LAN9118_RX_STS_BROADCAST) {
        s_p.stats.rx_broadcast++;
    }
    if (sts & LAN9118_RX_STS_MULTICAST) {
        s_p.stats.rx_multicast++;
    }
    if (sts & (LAN9118_RX_STS_CRC_ERROR | \
               LAN9118_RX_STS_FRAME_ERROR | \
               LAN9118_RX_STS_LEN_ERROR | \
               LAN9118_RX_STS_RX_ERR)) {
        s_p.stats.rx_errors++;
        if (sts & LAN9118_RX_STS_CRC_ERROR) {
            s_p.stats.rx_crc_errors++;
        }
        if (sts & LAN9118_RX_STS_FRAME_ERROR) {
            s_p.stats.rx_frame_errors++;
        }
    }
    return LAN9118_OK;
}

void lan9118_rx_process(void) {
    static uint8_t rx_buf[LAN9118_MAX_FRAME + 2];
    uint32_t len = 0;
    int budget = LAN9118_RX_MAX_FRAMES;
    if (!s_p.opened) {
        return;
    }
    while (lan9118_rx_pending() && (budget-- > 0)) {
        int ret = lan9118_read_frame(rx_buf, sizeof(rx_buf), &len);
        if (ret != LAN9118_OK) {
            break;
        }
        if (s_p.rx_cb != NULL) {
            s_p.rx_cb(rx_buf, len, s_p.rx_cb_arg);
        }
    }
}

void lan9118_link_poll(void) {
    bool up = false;
    if (!s_p.opened) {
        return;
    }
    up = lan9118_phy_link_up();
    if (up != s_p.link_up) {
        s_p.link_up = up;
        if (up) {
            s_p.stats.link_ups++;
            /* duplex may have changed */
            lan9118_mac_configure();
        } else {
            s_p.stats.link_downs++;
        }
        lan_dbg("link %s", up ? "up" : "down");
        if (s_p.link_cb != NULL) {
            s_p.link_cb(up, s_p.link_cb_arg);
        }
    }
}

void lan9118_isr(void) {
    uint32_t isr = 0;
    if (!s_p.opened) {
        return;
    }
    isr = lan_read(LAN9118_INT_STS);
    if (isr == 0U) {
        return;
    }
    s_p.stats.irq_count++;
    /* clear */
    lan_write(LAN9118_INT_STS, isr);
    if (isr & LAN9118_INT_RX_EVENTS) {
        /* Wake the stack worker (or set the poll flag on bare metal). */
        lan9118_osal_sem_give_from_isr();
    }
    if (isr & LAN9118_INT_TX_EVENTS) {
        lan9118_tx_status_process();
    }
    if (isr & LAN9118_INT_PHY_INT) {
        lan9118_link_poll();
    }
    if (isr & LAN9118_INT_FIFO_ERR) {
        if (isr & LAN9118_INT_RXE_INT) {
            s_p.stats.rx_overruns++;
        }
        if (isr & (LAN9118_INT_TDFO_INT | LAN9118_INT_TDFU_INT)) {
            s_p.stats.tx_fifo_overruns++;
        }
    }
}

/* NVIC IRQ 48 vector (see startup/startup_ARMCM33.s). */
void Interrupt48_Handler(void) {
    lan9118_isr();
}

int lan9118_link_status(void) {
    return s_p.link_up ? 1 : 0;
}

int lan9118_init(const lan9118_config_t *cfg) {
    int ret = 0;
    if (s_p.opened) {
        lan9118_close();
    }
    memset(&s_p, 0, sizeof(s_p));
    s_p.cfg.accept_broadcast = true;
    s_p.cfg.enable_irq = true;
    s_p.cfg.irq_prio = LAN9118_DEFAULT_IRQ_PRIO;
    s_p.cfg.tx_timeout_ms = LAN9118_DEFAULT_TX_TIMEOUT_MS;
    s_p.cfg.link_poll_ms = LAN9118_DEFAULT_LINK_POLL_MS;
    if (cfg != NULL) {
        /* Keep the defaults for fields the caller leaves at zero. */
        if (cfg->promiscuous) {
            s_p.cfg.promiscuous = true;
        }
        if (cfg->multicast) {
            s_p.cfg.multicast = true;
        }
        if (cfg->accept_broadcast) {
            s_p.cfg.accept_broadcast = true;
        }
        if (cfg->pass_bad_frames) {
            s_p.cfg.pass_bad_frames = true;
        }
        if (cfg->loopback) {
            s_p.cfg.loopback = true;
        }
        if (cfg->flow_control) {
            s_p.cfg.flow_control = true;
        }
        s_p.cfg.rx_offset = cfg->rx_offset;
        s_p.cfg.enable_irq = cfg->enable_irq;
        s_p.cfg.irq_prio = cfg->irq_prio;
        s_p.cfg.tx_timeout_ms = cfg->tx_timeout_ms;
        s_p.cfg.link_poll_ms = cfg->link_poll_ms;
        memcpy(s_p.cfg.mac, cfg->mac, 6);
    }
    /* OSAL (RX semaphore, ...) */
    (void)lan9118_osal_sem_init();
    /* Hardware reset + probe. */
    ret = lan9118_hw_reset();
    if (ret != LAN9118_OK) {
        return ret;
    }
    ret = lan9118_probe();
    if (ret != LAN9118_OK) {
        return ret;
    }
    /* MAC address: configuration -> EEPROM -> fallback. */
    if ((s_p.cfg.mac[0] | s_p.cfg.mac[1] | s_p.cfg.mac[2] |
         s_p.cfg.mac[3] | s_p.cfg.mac[4] | s_p.cfg.mac[5]) != 0U) {
        memcpy(s_p.mac, s_p.cfg.mac, 6);
    } else if (lan9118_mac_from_eeprom(s_p.mac)) {
        /* OK, MAC taken from EEPROM */
    } else {
        /* locally administered, avoids collisions */
        s_p.mac[0] = 0x02U;
        s_p.mac[1] = 0x00U;
        s_p.mac[2] = 0x00U;
        s_p.mac[3] = 0x00U;
        s_p.mac[4] = 0x00U;
        s_p.mac[5] = 0x01U;
    }
    ret = lan9118_mac_set_addr(s_p.mac);
    if (ret != LAN9118_OK) {
        return ret;
    }
    /* PHY bring-up (auto-negotiation). */
    ret = lan9118_phy_setup();
    if (ret != LAN9118_OK) {
        return ret;
    }
    /* Initial link state. */
    s_p.link_up = lan9118_phy_link_up();
    /* MAC / RX / flow-control / interrupts. */
    lan9118_mac_configure();
    lan9118_rx_configure();
    lan9118_afc_configure();
    lan9118_irq_setup();
    s_p.opened = true;
    return LAN9118_OK;
}

int lan9118_open(void) {
    return lan9118_init(NULL);
}

void lan9118_close(void) {
    if (!s_p.opened) {
        return;
    }
    lan9118_osal_irq_disable();
    lan_write(LAN9118_INT_EN, 0U);
    lan_write(LAN9118_INT_STS, 0xFFFFFFFFU);
    (void)lan9118_mac_write(LAN9118_MAC_CR, 0U);
    s_p.opened = false;
}

bool lan9118_is_open(void) {
    return s_p.opened;
}

int lan9118_set_mac(const uint8_t mac[6]) {
    int ret = 0;
    if (mac == NULL) {
        return LAN9118_ERR_PARAM;
    }
    if (!s_p.opened) {
        return LAN9118_ERR_NOT_READY;
    }
    ret = lan9118_mac_set_addr(mac);
    if (ret == LAN9118_OK) {
        memcpy(s_p.mac, mac, 6);
    }
    return ret;
}

void lan9118_get_mac(uint8_t mac[6]) {
    if (mac != NULL) {
        memcpy(mac, s_p.mac, 6);
    }
}

void lan9118_set_rx_callback(lan9118_rx_cb_t cb, void *arg) {
    uint32_t state = lan9118_osal_critical_enter();
    s_p.rx_cb = cb;
    s_p.rx_cb_arg = arg;
    lan9118_osal_critical_exit(state);
}

void lan9118_set_link_callback(lan9118_link_cb_t cb, void *arg) {
    uint32_t state = lan9118_osal_critical_enter();
    s_p.link_cb = cb;
    s_p.link_cb_arg = arg;
    lan9118_osal_critical_exit(state);
}

const lan9118_stats_t *lan9118_get_stats(void) {
    return &s_p.stats;
}

void lan9118_reset_stats(void) {
    uint32_t state = lan9118_osal_critical_enter();
    memset(&s_p.stats, 0, sizeof(s_p.stats));
    lan9118_osal_critical_exit(state);
}
