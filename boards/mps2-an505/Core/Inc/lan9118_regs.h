/**
 * @file    lan9118_regs.h
 * @brief   SMSC LAN9118 (software compatible with LAN9220) register map.
 *
 * All register offsets are relative to LAN9118_BASE and are accessed as
 * 32-bit little-endian words.
 *
 * On QEMU's "mps2-an505" machine the device is a LAN9118 mapped at
 * 0x42000000 with its interrupt wired to NVIC IRQ 48.  The bit
 * definitions below follow both the LAN9118 datasheet and the QEMU
 * device model (hw/net/lan9118.c), which is the reference for this
 * board.
 */
#ifndef LAN9118_REGS_H
#define LAN9118_REGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAN9118_BASE                        (0x42000000UL)
#define LAN9118_IRQn                        (48)

#define LAN9118_RX_DATA_FIFO                (0x00)
#define LAN9118_TX_DATA_FIFO                (0x20)
#define LAN9118_RX_STATUS_FIFO              (0x40)
#define LAN9118_RX_STATUS_PEEK              (0x44)
#define LAN9118_TX_STATUS_FIFO              (0x48)
#define LAN9118_TX_STATUS_PEEK              (0x4C)

#define LAN9118_ID_REV                      (0x50)
#define LAN9118_IRQ_CFG                     (0x54)
#define LAN9118_INT_STS                     (0x58)
#define LAN9118_INT_EN                      (0x5C)

#define LAN9118_BYTE_TEST                   (0x64)
#define LAN9118_FIFO_INT                    (0x68)
#define LAN9118_RX_CFG                      (0x6C)
#define LAN9118_TX_CFG                      (0x70)

#define LAN9118_HW_CFG                      (0x74)
#define LAN9118_RX_DP_CTRL                  (0x78)
#define LAN9118_RX_FIFO_INF                 (0x7C)
#define LAN9118_TX_FIFO_INF                 (0x80)

#define LAN9118_PMT_CTRL                    (0x84)
#define LAN9118_GPIO_CFG                    (0x88)
#define LAN9118_GPT_CFG                     (0x8C)
#define LAN9118_GPT_CNT                     (0x90)

#define LAN9118_ENDIAN                      (0x98)
#define LAN9118_FREE_RUN                    (0x9C)

#define LAN9118_RX_DROP                     (0xA0)
#define LAN9118_MAC_CSR_CMD                 (0xA4)
#define LAN9118_MAC_CSR_DATA                (0xA8)
#define LAN9118_AFC_CFG                     (0xAC)
#define LAN9118_E2P_CMD                     (0xB0)
#define LAN9118_E2P_DATA                    (0xB4)
/* deassert interval */
#define LAN9118_IRQ_CFG_INT_DEAS(x)         ((uint32_t)(x) << 20)
/* RO: IRQ pin level */
#define LAN9118_IRQ_CFG_IRQ_INT             (1UL << 12)
#define LAN9118_IRQ_CFG_IRQ_EN              (1UL <<  8)
/* 1 = active high */
#define LAN9118_IRQ_CFG_IRQ_POL             (1UL <<  4)
/* 1 = push-pull */
#define LAN9118_IRQ_CFG_IRQ_TYPE            (1UL <<  0)
/* software interrupt */
#define LAN9118_INT_SW_INT                  (1UL << 31)
/* TX stopped */
#define LAN9118_INT_TXSTOP_INT              (1UL << 25)
/* RX stopped */
#define LAN9118_INT_RXSTOP_INT              (1UL << 24)
/* RX data FIFO full */
#define LAN9118_INT_RXDFH_INT               (1UL << 23)
/* TX completed (IOC) */
#define LAN9118_INT_TX_IOC_INT              (1UL << 21)
/* RX data available */
#define LAN9118_INT_RXD_INT                 (1UL << 20)
/* general purpose timer */
#define LAN9118_INT_GPT_INT                 (1UL << 19)
/* PHY interrupt */
#define LAN9118_INT_PHY_INT                 (1UL << 18)
/* power management event */
#define LAN9118_INT_PME_INT                 (1UL << 17)
/* TX status overflow */
#define LAN9118_INT_TXSO_INT                (1UL << 16)
/* RX watchdog timeout */
#define LAN9118_INT_RWT_INT                 (1UL << 15)
/* RX error (FIFO underrun) */
#define LAN9118_INT_RXE_INT                 (1UL << 14)
/* TX error (FIFO underrun) */
#define LAN9118_INT_TXE_INT                 (1UL << 13)
/* TX data FIFO underrun */
#define LAN9118_INT_TDFU_INT                (1UL << 11)
/* TX data FIFO overrun */
#define LAN9118_INT_TDFO_INT                (1UL << 10)
/* TX data FIFO almost full */
#define LAN9118_INT_TDFA_INT                (1UL <<  9)
/* TX status FIFO full */
#define LAN9118_INT_TSFF_INT                (1UL <<  8)
/* TX status FIFO level */
#define LAN9118_INT_TSFL_INT                (1UL <<  7)
/* RX data FIFO level */
#define LAN9118_INT_RXDF_INT                (1UL <<  6)
/* RX data FIFO low */
#define LAN9118_INT_RDFL_INT                (1UL <<  5)
/* RX status FIFO full */
#define LAN9118_INT_RSFF_INT                (1UL <<  4)
/* RX status FIFO level */
#define LAN9118_INT_RSFL_INT                (1UL <<  3)
#define LAN9118_INT_GPIO2_INT               (1UL <<  2)
#define LAN9118_INT_GPIO1_INT               (1UL <<  1)
#define LAN9118_INT_GPIO0_INT               (1UL <<  0)

/* Grouping helpers */
#define LAN9118_INT_RX_EVENTS               (LAN9118_INT_RSFL_INT | LAN9118_INT_RSFF_INT | LAN9118_INT_RXDF_INT | LAN9118_INT_RDFL_INT | LAN9118_INT_RXDFH_INT | LAN9118_INT_RXD_INT)
#define LAN9118_INT_TX_EVENTS               (LAN9118_INT_TSFL_INT | LAN9118_INT_TSFF_INT | LAN9118_INT_TX_IOC_INT | LAN9118_INT_TXSO_INT)
#define LAN9118_INT_FIFO_ERR                (LAN9118_INT_RXE_INT | LAN9118_INT_TXE_INT | LAN9118_INT_TDFO_INT | LAN9118_INT_TDFU_INT | LAN9118_INT_TDFA_INT)

#define LAN9118_HW_CFG_SRST                 (1UL <<  0)
/* must be one */
#define LAN9118_HW_CFG_MBO                  (1UL <<  1)
/* 0..15 (x4KB) */
#define LAN9118_HW_CFG_TX_FIFO_SIZE(x)      ((uint32_t)(x) << 16)
/* 0..15 (x4KB) */
#define LAN9118_HW_CFG_RX_FIFO_SIZE(x)      ((uint32_t)(x) << 20)

/* 0..31 */
#define LAN9118_RX_CFG_RXDOFF(x)            ((uint32_t)(x) <<  8)
#define LAN9118_RX_CFG_RX_DMA_CNT(x)        ((uint32_t)(x) << 16)
/* flush RX FIFOs */
#define LAN9118_RX_CFG_RX_DUMP              (1UL << 15)
/* 4-byte alignment */
#define LAN9118_RX_CFG_RX_END_ALGN4         ((uint32_t)(0) << 30)

#define LAN9118_TX_CFG_TX_DMA_LEN           (1UL <<  1)
#define LAN9118_TX_CFG_TX_ON                (1UL <<  2)
#define LAN9118_TX_CFG_TXSTOP               (1UL << 14)
#define LAN9118_TX_CFG_TXSTS_DUMP           (1UL << 15)

#define LAN9118_FIFO_INT_RX_STATUS_LEVEL(x) ((uint32_t)(x) & 0xffU)
#define LAN9118_FIFO_INT_TX_STATUS_LEVEL(x) (((uint32_t)(x) & 0xffU) << 16)
/* incl. FCS */
#define LAN9118_RX_STS_PKT_LEN(x)           (((uint32_t)(x) >> 16) & 0x7ffU)
#define LAN9118_RX_STS_MII_ERROR            (1UL <<  0)
#define LAN9118_RX_STS_RX_ERR               (1UL <<  1)
#define LAN9118_RX_STS_FRAME_ERROR          (1UL <<  2)
#define LAN9118_RX_STS_CRC_ERROR            (1UL <<  3)
#define LAN9118_RX_STS_LEN_ERROR            (1UL <<  4)
#define LAN9118_RX_STS_PARSE_ERROR          (1UL <<  5)
#define LAN9118_RX_STS_FILT_FAIL            (1UL <<  6)
#define LAN9118_RX_STS_RUNT_PKT             (1UL <<  7)
#define LAN9118_RX_STS_CARRIER_EVNT         (1UL <<  8)
#define LAN9118_RX_STS_MULTICAST            (1UL << 10)
#define LAN9118_RX_STS_BROADCAST            (1UL << 13)
#define LAN9118_RX_STS_RX_END               (1UL << 15)
/* QEMU: frame failed filter */
#define LAN9118_RX_STS_FILT_FAIL_QEMU       (1UL << 30)

#define LAN9118_TX_CMD_A_FIRST_SEG          (1UL << 13)
#define LAN9118_TX_CMD_A_LAST_SEG           (1UL << 12)
#define LAN9118_TX_CMD_A_BUF_SIZE(x)        ((uint32_t)(x) & 0x7ffU)
/* ignore carrier */
#define LAN9118_TX_CMD_A_IC                 (1UL << 30)
/* interrupt on completion */
#define LAN9118_TX_CMD_A_IOC                (1UL << 31)

#define LAN9118_TX_CMD_B_DATA_SIZE(x)       (((uint32_t)(x) & 0x7ffU) << 16)
#define LAN9118_TX_CMD_B_BUF_SIZE(x)        ((uint32_t)(x) & 0x7ffU)
#define LAN9118_TX_CMD_B_END_ALGN           (1UL << 15)
#define LAN9118_TX_CMD_B_CSUM_ENABLE        (1UL << 14)
#define LAN9118_TX_CMD_B_CSUM_OFFLOAD       (1UL << 13)

#define LAN9118_TX_STS_DEFERRED             (1UL <<  0)
#define LAN9118_TX_STS_EXCESS_DEF           (1UL <<  1)
#define LAN9118_TX_STS_LATE_COLL            (1UL <<  2)
#define LAN9118_TX_STS_EXCESS_COLL          (1UL <<  3)
#define LAN9118_TX_STS_NO_CARRIER           (1UL <<  4)
#define LAN9118_TX_STS_LOSS_CARRIER         (1UL <<  5)
#define LAN9118_TX_STS_TX_ERR               (1UL << 11)

/* RX_FIFO_INF: [31:16] = number of RX status words (packets),
 *              [15:0]  = RX data FIFO used space in bytes */
#define LAN9118_RX_FIFO_INF_PKT_CNT(x)      (((uint32_t)(x) >> 16) & 0xffffU)
/* TX_FIFO_INF: [31:16] = TX status words, [15:0] = TX FIFO free space */
#define LAN9118_TX_FIFO_INF_FREE(x)         ((uint32_t)(x) & 0xffffU)

#define LAN9118_AFC_CFG_AFC_OFF             (1UL << 31)
#define LAN9118_AFC_CFG_VALID               (1UL << 30)
#define LAN9118_AFC_CFG_HI_LEVEL(x)         (((uint32_t)(x) & 0x0fU) << 16)
#define LAN9118_AFC_CFG_LO_LEVEL(x)         (((uint32_t)(x) & 0x0fU) <<  8)
#define LAN9118_AFC_CFG_BACK_DUR(x)         ((uint32_t)(x) & 0xffU)

#define LAN9118_E2P_CMD_E2P_BUSY            (1UL << 31)
#define LAN9118_E2P_CMD_MAC_ADDR_LOADED     (1UL << 8)
#define LAN9118_E2P_CMD_READ                (0UL << 28)
#define LAN9118_E2P_CMD_WRITE               (3UL << 28)
#define LAN9118_E2P_CMD_ERASE               (5UL << 28)
#define LAN9118_E2P_CMD_RELOAD              (7UL << 28)
#define LAN9118_E2P_CMD_ADDR(x)             ((uint32_t)(x) & 0x7fU)

#define LAN9118_PMT_CTRL_WOL_EN             (1UL <<  7)
#define LAN9118_PMT_CTRL_PME_EN             (1UL <<  5)
#define LAN9118_PMT_CTRL_READY              (1UL <<  3)
#define LAN9118_PMT_CTRL_ENERGYON           (1UL <<  2)
#define LAN9118_PMT_CTRL_PME_STATUS         (1UL <<  0)

#define LAN9118_MAC_CR                      (0x01)
#define LAN9118_MAC_ADDRH                   (0x02)
#define LAN9118_MAC_ADDRL                   (0x03)
#define LAN9118_MAC_HASHH                   (0x04)
#define LAN9118_MAC_HASHL                   (0x05)
#define LAN9118_MAC_MII_ACC                 (0x06)
#define LAN9118_MAC_MII_DATA                (0x07)
#define LAN9118_MAC_FLOW                    (0x08)

#define LAN9118_MAC_CSR_BUSY                (1UL << 31)
#define LAN9118_MAC_CSR_READ                (1UL << 30)
#define LAN9118_MAC_CSR_ADDR(x)             ((uint32_t)(x) & 0x0fU)

#define LAN9118_MAC_CR_RXALL                (1UL << 31)
#define LAN9118_MAC_CR_RCVOWN               (1UL << 23)
#define LAN9118_MAC_CR_LOOPBK               (1UL << 21)
#define LAN9118_MAC_CR_FDPX                 (1UL << 20)
/* pass all multicast */
#define LAN9118_MAC_CR_MCPAS                (1UL << 19)
/* promiscuous */
#define LAN9118_MAC_CR_PRMS                 (1UL << 18)
#define LAN9118_MAC_CR_INVFILT              (1UL << 17)
#define LAN9118_MAC_CR_PASSBAD              (1UL << 16)
#define LAN9118_MAC_CR_HO                   (1UL << 15)
#define LAN9118_MAC_CR_HPFILT               (1UL << 13)
#define LAN9118_MAC_CR_LCOLL                (1UL << 12)
/* 1 = reject broadcast */
#define LAN9118_MAC_CR_BCAST                (1UL << 11)
#define LAN9118_MAC_CR_DISRTY               (1UL << 10)
#define LAN9118_MAC_CR_PADSTR               (1UL <<  8)
#define LAN9118_MAC_CR_BOLMT                (3UL <<  6)
#define LAN9118_MAC_CR_DFCHK                (1UL <<  5)
#define LAN9118_MAC_CR_TXEN                 (1UL <<  3)
#define LAN9118_MAC_CR_RXEN                 (1UL <<  2)

#define LAN9118_MAC_FLOW_FCPT(x)            (((uint32_t)(x) & 0xffffU) << 16)
#define LAN9118_MAC_FLOW_FCPI(x)            (((uint32_t)(x) & 0xffU) << 8)
#define LAN9118_MAC_FLOW_RX_FLOW            (1UL << 2)
#define LAN9118_MAC_FLOW_TX_FLOW            (1UL << 1)

#define LAN9118_PHY_BCR                     (0x00)
#define LAN9118_PHY_BSR                     (0x01)
#define LAN9118_PHY_ID1                     (0x02)
#define LAN9118_PHY_ID2                     (0x03)
#define LAN9118_PHY_ANAR                    (0x04)
#define LAN9118_PHY_ANLPAR                  (0x05)
#define LAN9118_PHY_ANER                    (0x06)

#define LAN9118_PHY_BCR_RESET               (0x8000U)
#define LAN9118_PHY_BCR_LOOPBACK            (0x4000U)
#define LAN9118_PHY_BCR_SPEED100            (0x2000U)
#define LAN9118_PHY_BCR_AUTO_NEG            (0x1000U)
#define LAN9118_PHY_BCR_PDOWN               (0x0800U)
#define LAN9118_PHY_BCR_ISOLATE             (0x0400U)
#define LAN9118_PHY_BCR_AN_RESTART          (0x0200U)
#define LAN9118_PHY_BCR_FULL_DUPLEX         (0x0100U)

#define LAN9118_PHY_BSR_100TX_FD            (0x4000U)
#define LAN9118_PHY_BSR_100TX_HD            (0x2000U)
#define LAN9118_PHY_BSR_10T_FD              (0x1000U)
#define LAN9118_PHY_BSR_10T_HD              (0x0800U)
#define LAN9118_PHY_BSR_AUTO_NEG_COMP       (0x0020U)
#define LAN9118_PHY_BSR_REMOTE_FAULT        (0x0010U)
#define LAN9118_PHY_BSR_AUTO_NEG_ABIL       (0x0008U)
#define LAN9118_PHY_BSR_LINK_UP             (0x0004U)

#define LAN9118_PHY_ANLPAR_TXFD             (0x0100U)
#define LAN9118_PHY_ANLPAR_TX               (0x0080U)
#define LAN9118_PHY_ANLPAR_10FD             (0x0040U)
#define LAN9118_PHY_ANLPAR_10               (0x0020U)

#define LAN9118_MII_ACC_BUSY                (1UL << 0)
#define LAN9118_MII_ACC_WRITE               (1UL << 1)
#define LAN9118_MII_PHY_ADDR_SHIFT          (11)
#define LAN9118_MII_REG_ADDR_SHIFT          (6)
#define LAN9118_PHY_ADDR                    (1)

#define LAN9118_ID_REV_VALUE                (0x01180001UL)
#define LAN9118_BYTE_TEST_VALUE             (0x87654321UL)
#define LAN9118_SMSC_PHY_ID1                (0x0007U)
#define LAN9118_SMSC_PHY_ID2                (0xC0D1U)

#ifdef __cplusplus
}
#endif

#endif /* LAN9118_REGS_H */
