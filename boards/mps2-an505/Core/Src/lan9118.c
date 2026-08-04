#include "lan9118.h"

#define LAN9118_TX_TIMEOUT          (1000000U)
#define LAN9118_RX_MAX_LEN          (1518U)

#ifndef LAN9118_SOFTWARE_LOOPBACK
#define LAN9118_SOFTWARE_LOOPBACK   (0)
#endif

typedef struct {
    uint8_t data[LAN9118_RX_MAX_LEN];
    uint32_t len;
    bool pending;
    uint32_t status;
} lan9118_rx_fifo_t;

static lan9118_rx_fifo_t lan9118_rx_fifo;

static inline uint32_t lan_read(uint32_t reg) {
    return *(volatile uint32_t *)(LAN9118_BASE + reg);
}

static inline void lan_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(LAN9118_BASE + reg) = value;
}

static bool lan9118_wait_csr_ready(uint32_t reg, uint32_t mask) {
    uint32_t timeout = LAN9118_TX_TIMEOUT;
    while ((lan_read(reg) & mask) && timeout--) {
    }
    return timeout != 0;
}

static uint32_t lan9118_mac_read(uint8_t reg) {
    if (!lan9118_wait_csr_ready(LAN9118_MAC_CSR_CMD, MAC_CSR_BUSY)) {
        return 0;
    }
    lan_write(LAN9118_MAC_CSR_CMD, MAC_CSR_BUSY | MAC_CSR_READ | (uint32_t)reg);
    if (!lan9118_wait_csr_ready(LAN9118_MAC_CSR_CMD, MAC_CSR_BUSY)) {
        return 0;
    }
    return lan_read(LAN9118_MAC_CSR_DATA);
}

static void lan9118_mac_write(uint8_t reg, uint32_t value) {
    if (!lan9118_wait_csr_ready(LAN9118_MAC_CSR_CMD, MAC_CSR_BUSY)) {
        return;
    }
    lan_write(LAN9118_MAC_CSR_DATA, value);
    lan_write(LAN9118_MAC_CSR_CMD, MAC_CSR_BUSY | (uint32_t)reg);
    (void)lan9118_wait_csr_ready(LAN9118_MAC_CSR_CMD, MAC_CSR_BUSY);
}

static void lan9118_dump_mac(void) {
    uint32_t high = 0, low = 0;
    uint8_t mac[6] = { 0 };
    high = lan9118_mac_read(MAC_ADDRH);
    low  = lan9118_mac_read(MAC_ADDRL);
    mac[0] = (low  & 0xff);
    mac[1] = ((low >>  8) & 0xff);
    mac[2] = ((low >> 16) & 0xff);
    mac[3] = ((low >> 24) & 0xff);
    mac[4] = (high & 0xff);
    mac[5] = ((high >> 8) & 0xff);
    printf("MAC = %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int16_t lan9118_phy_read(uint8_t reg) {
    uint32_t cmd = 0;
    while (lan9118_mac_read(MAC_MII_ACC) & MII_ACC_BUSY) {}
    cmd = (PHY_ADDR << MII_PHY_ADDR_SHIFT) | (reg << MII_REG_ADDR_SHIFT);
    lan9118_mac_write(MAC_MII_ACC, cmd);
    while (lan9118_mac_read(MAC_MII_ACC) & MII_ACC_BUSY) {}
    return lan9118_mac_read(MAC_MII_DATA) & 0xffff;
}

static void lan9118_phy_write(uint8_t reg, uint16_t value) {
    uint32_t cmd = 0;
    while (lan9118_mac_read(MAC_MII_ACC) & MII_ACC_BUSY) {}
    lan9118_mac_write(MAC_MII_DATA, value);
    cmd = MII_ACC_WRITE | (PHY_ADDR << MII_PHY_ADDR_SHIFT) | (reg << MII_REG_ADDR_SHIFT);
    lan9118_mac_write(MAC_MII_ACC, cmd);
    while (lan9118_mac_read(MAC_MII_ACC) & MII_ACC_BUSY) {}
}

static void lan9118_dump_phy(void) {
    printf("PHY ID1 = %04X\n", lan9118_phy_read(PHY_ID1));
    printf("PHY ID2 = %04X\n", lan9118_phy_read(PHY_ID2));
    printf("PHY BSR = %04X\n", lan9118_phy_read(PHY_BSR));
}

static bool lan9118_probe(void) {
    uint32_t id = lan_read(LAN9118_ID_REV);
    printf("LAN9118 ID = %08lX\n", id);
    if (id == 0 || id == 0xffffffff) {
        return false;
    }
    printf("BYTE_TEST = %08lX\n", lan_read(LAN9118_BYTE_TEST));
    printf("HW_CFG = %08lX\n", lan_read(LAN9118_HW_CFG));
    return true;
}

static void lan9118_reset(void) {
    lan_write(LAN9118_HW_CFG, lan_read(LAN9118_HW_CFG) | HW_CFG_SRST);
    while (lan_read(LAN9118_HW_CFG) & HW_CFG_SRST) {}
}

static void lan9118_mac_enable(void) {
    uint32_t cr = 0;
    cr = lan9118_mac_read(MAC_CR);
    cr |= MAC_CR_TXEN | MAC_CR_RXEN;
    lan9118_mac_write(MAC_CR, cr);
    printf("MAC_CR = %08lX\n", lan9118_mac_read(MAC_CR));
}

static bool lan9118_link_up(void) {
    return (lan9118_phy_read(PHY_BSR) & (1U << 2)) != 0U;
}

static uint32_t lan9118_irq_status(void) {
    return lan_read(LAN9118_INT_STS);
}

static void lan9118_irq_enable(uint32_t mask) {
    lan_write(LAN9118_INT_EN, mask);
}

static void lan9118_irq_clear(uint32_t mask) {
    lan_write(LAN9118_INT_STS, mask);
}

static void lan9118_rx_fifo_push(const uint8_t *data, uint32_t len) {
    if (!data || len == 0U || len > LAN9118_RX_MAX_LEN) {
        return;
    }
    memcpy(lan9118_rx_fifo.data, data, len);
    lan9118_rx_fifo.len = len;
    lan9118_rx_fifo.pending = true;
    lan9118_rx_fifo.status = 0x00000001U | ((len & 0x7ffU) << 16);
}

static bool lan9118_hw_rx_read(uint8_t *data, uint32_t *len) {
    uint32_t status = 0;
    uint32_t pkt_len = 0;
    uint32_t rx_fifo_inf = 0;
    if (!data || !len) {
        return false;
    }
    status = lan_read(LAN9118_RX_STATUS_FIFO);
    rx_fifo_inf = lan_read(LAN9118_RX_FIFO_INF);
    printf("RX_STATUS_FIFO=%08lX\n", (unsigned long)status);
    printf("RX_FIFO_INF=%08lX\n", (unsigned long)rx_fifo_inf);
    if (status == 0U || status == 0xFFFFFFFFU) {
        return false;
    }
    pkt_len = status & 0x7ffU;
    if (pkt_len < 14U || pkt_len > LAN9118_RX_MAX_LEN) {
        printf("RX frame length invalid: %lu\n", (unsigned long)pkt_len);
        return false;
    }
    for (uint32_t i = 0; i < pkt_len; i += 4U) {
        uint32_t word = lan_read(LAN9118_RX_DATA_FIFO);
        uint32_t n = pkt_len - i;
        if (n > 4U) {
            n = 4U;
        }
        memcpy(&data[i], &word, n);
    }
    *len = pkt_len;
    return true;
}

static int lan9118_rx_fifo_pop(uint8_t *data, uint32_t *len) {
    if (!data || !len) {
        return -1;
    }
    if (!lan9118_rx_fifo.pending) {
        *len = 0;
        return -2;
    }
    *len = lan9118_rx_fifo.len;
    memcpy(data, lan9118_rx_fifo.data, *len);
    lan9118_rx_fifo.pending = false;
    lan9118_rx_fifo.len = 0;
    lan9118_rx_fifo.status = 0;
    return 0;
}

static void lan9118_rx_init(void) {
    lan_write(LAN9118_RX_CFG, RX_CFG_RXDOFF(0) | RX_CFG_RX_END_ALGN4);
    lan_write(LAN9118_RX_DROP, 0);
    lan_write(LAN9118_INT_EN, 0U);
    lan_write(LAN9118_INT_STS, 0xFFFFFFFFU);
    (void)lan_read(LAN9118_RX_STATUS_FIFO);
    (void)lan_read(LAN9118_RX_DATA_FIFO);
    lan9118_rx_fifo.len = 0;
    lan9118_rx_fifo.pending = false;
    lan9118_rx_fifo.status = 0;
}

static bool lan9118_tx_ready(void) {
    uint32_t info = 0;
    info = lan_read(LAN9118_TX_FIFO_INF);
    printf("TX_FIFO_INF=%08lX\n", info);
    return (info != 0xFFFFFFFFU);
}

static bool lan9118_init(void) {
    uint32_t bsr = 0;
    lan9118_reset();
    if (!lan9118_probe()) {
        return false;
    }
    lan9118_mac_enable();
    lan9118_rx_init();
    lan9118_irq_enable(0U);
    lan9118_irq_clear(0xFFFFFFFFU);
    /* Basic PHY bring-up: reset and advertise full-duplex/100Mbps capability. */
    lan9118_phy_write(PHY_BCR, 0x8000U);
    lan9118_phy_write(PHY_BCR, 0x2100U);
    bsr = lan9118_phy_read(PHY_BSR);
    printf("PHY_BSR = %04X\n", (unsigned int)bsr);
    lan9118_dump_mac();
    lan9118_dump_phy();
    return true;
}

int lan9118_open(void) {
    return lan9118_init() ? 0 : -1;
}

int lan9118_send(const uint8_t *data, uint32_t len) {
    uint32_t cmd_a = 0;
    uint32_t cmd_b = 0;
    uint32_t padded = 0;
    if (!data || len < 14U || len > LAN9118_RX_MAX_LEN) {
        return -1;
    }
    while (!lan9118_tx_ready()) {}
    printf("TX ready\n");
    cmd_a = TX_CMD_A_FIRST_SEG | TX_CMD_A_LAST_SEG | (len & 0x7ffU);
    lan_write(LAN9118_TX_DATA_FIFO, cmd_a);
    cmd_b = ((len & 0x7ffU) << 16) | (len & 0x7ffU);
    lan_write(LAN9118_TX_DATA_FIFO, cmd_b);
    padded = (len + 3U) & ~3U;
    for (uint32_t i = 0; i < padded; i += 4U) {
        uint32_t v = 0;
        if (i < len) {
            uint32_t n = len - i;
            if (n > 4U) {
                n = 4U;
            }
            memcpy(&v, &data[i], n);
        }
        lan_write(LAN9118_TX_DATA_FIFO, v);
    }
    if (LAN9118_SOFTWARE_LOOPBACK) {
        lan9118_rx_fifo_push(data, len);
    }
    printf("TX done\n");
    return 0;
}

int lan9118_receive(uint8_t *data, uint32_t *len) {
    if (!data || !len) {
        return -1;
    }
    if (lan9118_hw_rx_read(data, len)) {
        return 0;
    }
    if (LAN9118_SOFTWARE_LOOPBACK) {
        return lan9118_rx_fifo_pop(data, len);
    }
    *len = 0;
    return -2;
}

int lan9118_xmit(const uint8_t *data, uint32_t len) {
    return lan9118_send(data, len);
}

int lan9118_poll(uint8_t *data, uint32_t *len) {
    return lan9118_receive(data, len);
}

void lan9118_poll_loop(void) {
    static uint8_t rx_buf[LAN9118_RX_MAX_LEN] = { 0 };
    uint32_t rx_len = 0;
    int ret = 0;
    ret = lan9118_poll(rx_buf, &rx_len);
    if (ret == 0 && rx_len > 0U) {
        printf("Poll RX size: %lu\n", (unsigned long)rx_len);
        printf("Poll RX data:\n");
        for (uint32_t i = 0; i < rx_len; i++) {
            printf(" %02X ", rx_buf[i]);
            if ((i + 1U) % 16U == 0U) {
                printf("\n");
            }
        }
        printf("\n");
    }
}

void lan9118_isr(void) {
    uint32_t irq = lan9118_irq_status();
    if (irq != 0U) {
        printf("LAN9118 IRQ = %08lX\n", (unsigned long)irq);
        lan9118_irq_clear(irq);
    }
}

void lan9118_test(void) {
    uint32_t rx_len = 0;
    static uint8_t tx_pkt[60] = { 0 };
    static uint8_t rx_buf[LAN9118_RX_MAX_LEN] = { 0 };
    memset(tx_pkt, 0xFF, sizeof(tx_pkt));
    tx_pkt[0x06] = 0x52;
    tx_pkt[0x07] = 0x54;
    tx_pkt[0x08] = 0x00;
    tx_pkt[0x09] = 0x12;
    tx_pkt[0x0A] = 0x34;
    tx_pkt[0x0B] = 0x56;
    tx_pkt[0x0C] = 0x08;
    tx_pkt[0x0D] = 0x06;
    printf("LAN9118 loopback=%s\n", LAN9118_SOFTWARE_LOOPBACK ? "on" : "off");
    printf("PHY BSR = %04X, link=%s\n", lan9118_phy_read(PHY_BSR), lan9118_link_up() ? "up" : "down");
    printf("IRQ_STS = %08lX\n", lan_read(LAN9118_INT_STS));
    if (lan9118_send(tx_pkt, sizeof(tx_pkt)) == 0) {
        if (lan9118_receive(rx_buf, &rx_len) == 0) {
            printf("RX size: %lu\n", (unsigned long)rx_len);
            printf("RX data:\n");
            for (uint32_t i = 0; i < rx_len; i++) {
                printf("%02X ", rx_buf[i]);
                if ((i + 1) % 16 == 0) {
                    printf(" \n");
                }
            }
            printf(" \n");
        } else {
            printf("No RX frame available\n");
        }
    }
    printf("LAN9118_INT_STS = %08lX\n", lan_read(LAN9118_INT_STS));
    printf("LAN9118_TX_STATUS_FIFO = %08lX\n", lan_read(LAN9118_TX_STATUS_FIFO));
}
