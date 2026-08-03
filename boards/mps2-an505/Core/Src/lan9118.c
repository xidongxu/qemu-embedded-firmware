#include "lan9118.h"

static inline uint32_t lan_read(uint32_t reg) {
    return *(volatile uint32_t *)(LAN9118_BASE + reg);
}

static inline void lan_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(LAN9118_BASE + reg) = value;
}

uint32_t lan9118_mac_read(uint8_t reg) {
    while (lan_read(LAN9118_MAC_CSR_CMD) & MAC_CSR_BUSY);
    lan_write(LAN9118_MAC_CSR_CMD, MAC_CSR_BUSY | MAC_CSR_READ | reg);
    while (lan_read(LAN9118_MAC_CSR_CMD) & MAC_CSR_BUSY);
    return lan_read(LAN9118_MAC_CSR_DATA);
}

void lan9118_mac_write(uint8_t reg, uint32_t value) {
    while (lan_read(LAN9118_MAC_CSR_CMD) & MAC_CSR_BUSY);
    lan_write(LAN9118_MAC_CSR_DATA, value);
    lan_write(LAN9118_MAC_CSR_CMD, MAC_CSR_BUSY | reg);
    while (lan_read(LAN9118_MAC_CSR_CMD) & MAC_CSR_BUSY);
}

void lan9118_dump_mac(void) {
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

int16_t lan9118_phy_read(uint8_t reg) {
    uint32_t cmd = 0;
    while (lan9118_mac_read(MAC_MII_ACC) & MII_ACC_BUSY) {}
    cmd = (PHY_ADDR << MII_PHY_ADDR_SHIFT) | (reg << MII_REG_ADDR_SHIFT);
    lan9118_mac_write(MAC_MII_ACC, cmd);
    while (lan9118_mac_read(MAC_MII_ACC) & MII_ACC_BUSY) {}
    return lan9118_mac_read(MAC_MII_DATA) & 0xffff;
}

void lan9118_phy_write(uint8_t reg, uint16_t value) {
    uint32_t cmd = 0;
    while (lan9118_mac_read(MAC_MII_ACC) & MII_ACC_BUSY) {}
    lan9118_mac_write(MAC_MII_DATA, value);
    cmd = MII_ACC_WRITE | (PHY_ADDR << MII_PHY_ADDR_SHIFT) | (reg << MII_REG_ADDR_SHIFT);
    lan9118_mac_write(MAC_MII_ACC, cmd);
    while (lan9118_mac_read(MAC_MII_ACC) & MII_ACC_BUSY) {}
}

void lan9118_dump_phy(void) {
    printf("PHY ID1 = %04X\n", lan9118_phy_read(PHY_ID1));
    printf("PHY ID2 = %04X\n", lan9118_phy_read(PHY_ID2));
    printf("PHY BSR = %04X\n", lan9118_phy_read(PHY_BSR));
}

bool lan9118_probe(void) {
    uint32_t id = lan_read(LAN9118_ID_REV);
    printf("LAN9118 ID = %08lX\n", id);
    if (id == 0 || id == 0xffffffff) {
        return false;
    }
    printf("BYTE_TEST = %08lX\n", lan_read(LAN9118_BYTE_TEST));
    printf("HW_CFG = %08lX\n", lan_read(LAN9118_HW_CFG));
    return true;
}

void lan9118_reset(void) {
    lan_write(LAN9118_HW_CFG, lan_read(LAN9118_HW_CFG) | HW_CFG_SRST);
    while (lan_read(LAN9118_HW_CFG) & HW_CFG_SRST) {}
}

void lan9118_mac_enable(void) {
    uint32_t cr = 0;
    cr = lan9118_mac_read(MAC_CR);
    cr |= MAC_CR_TXEN | MAC_CR_RXEN;
    lan9118_mac_write(MAC_CR, cr);
    printf("MAC_CR = %08lX\n", lan9118_mac_read(MAC_CR));
}

void lan9118_rx_init(void) {
    lan_write(LAN9118_RX_CFG, 0);
}

static bool lan9118_tx_ready(void) {
    uint32_t info = 0;
    info = lan_read(LAN9118_TX_FIFO_INF);
    printf("TX_FIFO_INF=%08lX\n", info);
    return true;
}

int lan9118_send(const uint8_t *data, uint32_t len) {
    uint32_t cmd_a = 0;
    uint32_t cmd_b = 0;
    if(len < 14) {
        return -1;
    }
    while(!lan9118_tx_ready()) {}
    printf("TX ready\n");
    cmd_a = TX_CMD_A_FIRST_SEG | TX_CMD_A_LAST_SEG | len;
    lan_write(LAN9118_TX_DATA_FIFO, cmd_a);
    cmd_b = ((len & 0x7ff) << 16) | (len & 0x7ff);
    lan_write(LAN9118_TX_DATA_FIFO, cmd_b);
    uint32_t padded = (len + 3) & ~3;
    for(uint32_t i = 0;i < padded; i += 4) {
        uint32_t v = 0;
        if(i < len) {
            uint32_t n = len - i;
            if(n > 4) {
                n = 4;
            }
            memcpy(&v, &data[i], n);
        }
        lan_write(LAN9118_TX_DATA_FIFO, v);
    }
    printf("TX done\n");
    return 0;
}

void lan9118_test(void) {
    uint8_t pkt[60] = { 0 };
    memset(pkt, 0xff, sizeof(pkt));
    pkt[6]=0x52;
    pkt[7]=0x54;
    pkt[8]=0x00;
    pkt[9]=0x12;
    pkt[10]=0x34;
    pkt[11]=0x56;
    pkt[12]=0x08;
    pkt[13]=0x06;
    lan9118_send(pkt, 60);
    printf("LAN9118_INT_STS = %08lX\n", lan_read(LAN9118_INT_STS));
    printf("LAN9118_TX_STATUS_FIFO = %08lX\n", lan_read(LAN9118_TX_STATUS_FIFO));
}
