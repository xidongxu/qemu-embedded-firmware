#ifndef LAN9118_REGS_H
#define LAN9118_REGS_H

#include <stdint.h>

#define LAN9118_BASE          0x42000000UL

/* CSR Registers */
#define LAN9118_RX_DATA_FIFO      0x00
#define LAN9118_TX_DATA_FIFO      0x20
#define LAN9118_RX_STATUS_FIFO    0x40
#define LAN9118_RX_STATUS_PEEK    0x44
#define LAN9118_TX_STATUS_FIFO    0x48
#define LAN9118_TX_STATUS_PEEK    0x4C

#define LAN9118_ID_REV            0x50
#define LAN9118_IRQ_CFG           0x54
#define LAN9118_INT_STS           0x58
#define LAN9118_INT_EN            0x5C

#define LAN9118_BYTE_TEST         0x64
#define LAN9118_FIFO_INT          0x68
#define LAN9118_RX_CFG            0x6C
#define LAN9118_TX_CFG            0x70

#define LAN9118_HW_CFG            0x74
#define LAN9118_RX_DP_CTRL        0x78
#define LAN9118_RX_FIFO_INF       0x7C
#define LAN9118_TX_FIFO_INF       0x80

#define LAN9118_PMT_CTRL          0x84
#define LAN9118_GPIO_CFG          0x88
#define LAN9118_GPT_CFG           0x8C
#define LAN9118_GPT_CNT           0x90

#define LAN9118_ENDIAN            0x98
#define LAN9118_FREE_RUN          0x9C

#define LAN9118_RX_DROP           0xA0
#define LAN9118_MAC_CSR_CMD       0xA4
#define LAN9118_MAC_CSR_DATA      0xA8
#define LAN9118_AFC_CFG           0xAC
#define LAN9118_E2P_CMD           0xB0
#define LAN9118_E2P_DATA          0xB4

/* MAC CSR address */
#define MAC_CR                    0x01
#define MAC_ADDRH                 0x02
#define MAC_ADDRL                 0x03
#define MAC_HASHH                 0x04
#define MAC_HASHL                 0x05
#define MAC_MII_ACC               0x06
#define MAC_MII_DATA              0x07
#define FLOW                      0x08

/* MAC CSR CMD bits */
#define MAC_CSR_BUSY              (1 << 31)
#define MAC_CSR_READ              (1 << 30)

/* PHY Registers */
#define PHY_BCR                   0x00
#define PHY_BSR                   0x01
#define PHY_ID1                   0x02
#define PHY_ID2                   0x03

#define MII_ACC_BUSY              (1 << 0)
#define MII_ACC_WRITE             (1 << 1)

#define MII_PHY_ADDR_SHIFT        11
#define MII_REG_ADDR_SHIFT        6
#define PHY_ADDR                  1

#define MAC_CR_RXEN               0x00000004
#define MAC_CR_TXEN               0x00000008

#define RX_CFG_RXDOFF(x)          ((x) <<  8)
#define RX_CFG_RX_DMA_CNT(x)      ((x) << 16)
#define RX_CFG_RX_END_ALGN4       ((0) << 30)

#define TX_CMD_A_FIRST_SEG        (1 << 13)
#define TX_CMD_A_LAST_SEG         (1 << 12)
#define TX_CMD_B_FIRST_SEG        (1 << 13)
#define TX_CMD_B_LAST_SEG         (1 << 12)

#define HW_CFG_SRST               (1 << 0)

#endif
