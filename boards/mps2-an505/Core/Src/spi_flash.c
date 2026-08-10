/*
 * spi_flash.c - industrial SPI NOR flash driver for the mps2-an505 (QEMU).
 *
 * Hardware: PL022 SSP0 @ 0x40205000 (8-bit, master mode) with a Winbond
 * w25q02jvm (256 MiB) on its SSI bus.  The flash chip-select is driven by
 * bit 8 of the MPS2 FPGAIO MISC register (0x4030204c):
 *      0 = assert / select, 1 = deassert / release.
 *
 * Because the part is larger than 16 MiB, the driver uses the 4-byte
 * address command set (READ4 0x13, PP4 0x12, ERASE4_4K 0x21, ERASE4_32K
 * 0x5c, ERASE4_SECTOR 0xdc) and never touches the non-volatile
 * "enter 4-byte mode" configuration, so it works regardless of the chip's
 * current addressing mode.
 *
 * Thread safety: every command transaction runs inside a short critical
 * section (interrupts off, PRIMASK preserved) so a single CS-assert ->
 * transfer -> CS-release sequence can never be torn by ISRs.  Whole
 * read/write/erase operations can additionally be serialised between tasks
 * via the weak spi_flash_os_lock()/spi_flash_os_unlock() hooks.
 */

#include <string.h>

#include "ARMCM33_DSP_FP.h"
#include "spi_flash.h"
/* only used by spi_flash_selftest() */
#include "printf.h"

#define PL0220_BASE       (0x40205000UL)
#define PL022_CR0         (*(volatile uint32_t *)(PL0220_BASE + 0x000))
#define PL022_CR1         (*(volatile uint32_t *)(PL0220_BASE + 0x004))
#define PL022_DR          (*(volatile uint32_t *)(PL0220_BASE + 0x008))
#define PL022_SR          (*(volatile uint32_t *)(PL0220_BASE + 0x00c))
#define PL022_CPSR        (*(volatile uint32_t *)(PL0220_BASE + 0x010))

#define FPGAIO_MISC       (*(volatile uint32_t *)(0x40302000UL + 0x4c))
/* 0 = select, 1 = release */
#define SPI_CS_BIT        (1U << 8)
#define SR_TFE            (1U << 0)
#define SR_TNF            (1U << 1)
#define SR_RNE            (1U << 2)
#define SR_RFF            (1U << 3)
#define SR_BSY            (1U << 4)

#define CMD_JEDEC_ID      (0x9fU)
#define CMD_WREN          (0x06U)
#define CMD_WRDI          (0x04U)
#define CMD_RDSR          (0x05U)
#define CMD_READ_4B       (0x13U)
#define CMD_PP_4B         (0x12U)
#define CMD_ERASE_4K_4B   (0x21U)
#define CMD_ERASE_32K_4B  (0x5cU)
#define CMD_ERASE_64K_4B  (0xdcU)
#define CMD_ERASE_CHIP    (0xc7U)

#define SR_BUSY           (1U << 0)

/* CPU clock used to convert DWT ticks to milliseconds (QEMU: 20 MHz). */
#define SPI_FLASH_CPU_HZ  (20000000UL)

static bool             s_init;
static spi_flash_info_t s_info;
static uint32_t         s_poll_timeout_ms = 1000U;

/* forward declarations used by spi_flash_wait_busy() */
static uint32_t spi_flash_xfer(uint32_t tx);
static void     spi_flash_cs(bool assert);

__attribute__((weak)) void spi_flash_os_lock(void)   { }
__attribute__((weak)) void spi_flash_os_unlock(void) { }

static void spi_flash_dwt_enable(void) {
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0U;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
}

/*
 * Poll the flash status register until BUSY clears or the timeout elapses.
 * A 32-bit unsigned wrap-around-safe deadline comparison is used.
 */
static spi_flash_err_t spi_flash_wait_busy(void) {
    uint32_t deadline = DWT->CYCCNT +
        (uint32_t)((uint64_t)s_poll_timeout_ms * (SPI_FLASH_CPU_HZ / 1000U));

    for (;;) {
        uint32_t sr;
        spi_flash_cs(true);
        spi_flash_xfer(CMD_RDSR);
        sr = spi_flash_xfer(0U);
        spi_flash_cs(false);

        if ((sr & SR_BUSY) == 0U) {
            return SPI_FLASH_OK;
        }
        if ((int32_t)(DWT->CYCCNT - deadline) >= 0) {
            return SPI_FLASH_ERR_TIMEOUT;
        }
    }
}

static uint32_t spi_flash_xfer(uint32_t tx) {
    while ((PL022_SR & SR_TNF) == 0U) { }
    PL022_DR = tx;
    while ((PL022_SR & SR_RNE) == 0U) { }
    return PL022_DR & 0xFFU;
}

static void spi_flash_cs(bool assert) {
    if (assert) {
        /* bit8 = 0 -> select */
        FPGAIO_MISC &= ~SPI_CS_BIT;
    } else {
        /* bit8 = 1 -> release */
        FPGAIO_MISC |= SPI_CS_BIT;
    }
    __DMB();
}

/* Critical section preserving PRIMASK (safe even when IRQs already off). */
static void spi_flash_critical_enter(uint32_t *primask) {
    *primask = __get_PRIMASK();
    __disable_irq();
}

static void spi_flash_critical_exit(uint32_t primask) {
    if ((primask & 1U) == 0U) {
        __enable_irq();
    }
}

static spi_flash_err_t spi_flash_cmd_jedec(uint8_t id[3]) {
    uint32_t primask;

    spi_flash_critical_enter(&primask);
    spi_flash_cs(true);
    spi_flash_xfer(CMD_JEDEC_ID);
    id[0] = (uint8_t)spi_flash_xfer(0U);
    id[1] = (uint8_t)spi_flash_xfer(0U);
    id[2] = (uint8_t)spi_flash_xfer(0U);
    spi_flash_cs(false);
    spi_flash_critical_exit(primask);

    return SPI_FLASH_OK;
}

static spi_flash_err_t spi_flash_cmd_read4(uint32_t addr,
                                           uint8_t *dst, uint32_t len) {
    uint32_t primask;

    spi_flash_critical_enter(&primask);
    spi_flash_cs(true);
    spi_flash_xfer(CMD_READ_4B);
    spi_flash_xfer(addr >> 24);
    spi_flash_xfer(addr >> 16);
    spi_flash_xfer(addr >> 8);
    spi_flash_xfer(addr);
    for (uint32_t i = 0U; i < len; i++) {
        dst[i] = (uint8_t)spi_flash_xfer(0U);
    }
    spi_flash_cs(false);
    spi_flash_critical_exit(primask);

    return SPI_FLASH_OK;
}

static spi_flash_err_t spi_flash_cmd_page_program4(uint32_t addr,
                                                   const uint8_t *src,
                                                   uint32_t len) {
    uint32_t primask;

    spi_flash_critical_enter(&primask);
    /* WREN */
    spi_flash_cs(true);
    spi_flash_xfer(CMD_WREN);
    spi_flash_cs(false);
    /* page program */
    spi_flash_cs(true);
    spi_flash_xfer(CMD_PP_4B);
    spi_flash_xfer(addr >> 24);
    spi_flash_xfer(addr >> 16);
    spi_flash_xfer(addr >> 8);
    spi_flash_xfer(addr);
    for (uint32_t i = 0U; i < len; i++) {
        spi_flash_xfer(src[i]);
    }
    /* release commits the page */
    spi_flash_cs(false);
    spi_flash_critical_exit(primask);

    return spi_flash_wait_busy();
}

static spi_flash_err_t spi_flash_cmd_erase(uint8_t cmd, uint32_t addr) {
    uint32_t primask;

    spi_flash_critical_enter(&primask);
    /* WREN */
    spi_flash_cs(true);
    spi_flash_xfer(CMD_WREN);
    spi_flash_cs(false);
    /* erase */
    spi_flash_cs(true);
    spi_flash_xfer(cmd);
    spi_flash_xfer(addr >> 24);
    spi_flash_xfer(addr >> 16);
    spi_flash_xfer(addr >> 8);
    spi_flash_xfer(addr);
    spi_flash_cs(false);
    spi_flash_critical_exit(primask);

    return spi_flash_wait_busy();
}

static spi_flash_err_t spi_flash_cmd_chip_erase(void) {
    uint32_t primask;

    spi_flash_critical_enter(&primask);
    spi_flash_cs(true);
    spi_flash_xfer(CMD_WREN);
    spi_flash_cs(false);
    spi_flash_cs(true);
    spi_flash_xfer(CMD_ERASE_CHIP);
    spi_flash_cs(false);
    spi_flash_critical_exit(primask);

    return spi_flash_wait_busy();
}

spi_flash_err_t spi_flash_init(const spi_flash_config_t *cfg) {
    uint8_t id[3];

    if (cfg != NULL && cfg->poll_timeout_ms != 0U) {
        s_poll_timeout_ms = cfg->poll_timeout_ms;
    }

    /* Configure PL022: 8-bit, SPI master (SSE=1, MS=0). */
    PL022_CR1 = 0x00U;
    /* DSS = 7 -> 8-bit */
    PL022_CR0 = 0x07U;
    PL022_CPSR = 0x02U;
    /* SSE enable, master */
    PL022_CR1 = 0x02U;
    __DMB();

    spi_flash_dwt_enable();

    /* Probe the part. */
    spi_flash_cmd_jedec(id);
    if (id[0] != SPI_FLASH_JEDEC_MFR ||
        id[1] != SPI_FLASH_JEDEC_TYPE ||
        id[2] != SPI_FLASH_JEDEC_CAP) {
        return SPI_FLASH_ERR_PROBE;
    }

    s_info.jedec[0] = id[0];
    s_info.jedec[1] = id[1];
    s_info.jedec[2] = id[2];
    s_info.size = SPI_FLASH_SIZE;
    s_info.page_size = SPI_FLASH_PAGE_SIZE;
    s_info.sector_size = SPI_FLASH_SECTOR_SIZE;
    s_info.block_size = SPI_FLASH_BLOCK_SIZE;
    s_info.four_byte_addr = true;

    s_init = true;
    return SPI_FLASH_OK;
}

spi_flash_err_t spi_flash_deinit(void) {
    if (!s_init) {
        return SPI_FLASH_ERR_NOT_INIT;
    }
    /* release CS, disable the SPI engine */
    spi_flash_cs(false);
    PL022_CR1 = 0x00U;
    __DMB();
    s_init = false;
    return SPI_FLASH_OK;
}

spi_flash_err_t spi_flash_get_info(spi_flash_info_t *info) {
    if (!s_init) {
        return SPI_FLASH_ERR_NOT_INIT;
    }
    if (info == NULL) {
        return SPI_FLASH_ERR_PARAM;
    }
    *info = s_info;
    return SPI_FLASH_OK;
}

spi_flash_err_t spi_flash_read(uint32_t addr, void *buf, uint32_t len) {
    if (!s_init) {
        return SPI_FLASH_ERR_NOT_INIT;
    }
    if (buf == NULL || len == 0U) {
        return SPI_FLASH_ERR_PARAM;
    }
    if (addr > SPI_FLASH_MAX_ADDR || len > (SPI_FLASH_SIZE - addr)) {
        return SPI_FLASH_ERR_PARAM;
    }

    spi_flash_os_lock();
    spi_flash_err_t rc = spi_flash_cmd_read4(addr, (uint8_t *)buf, len);
    spi_flash_os_unlock();
    return rc;
}

spi_flash_err_t spi_flash_write(uint32_t addr, const void *buf, uint32_t len) {
    if (!s_init) {
        return SPI_FLASH_ERR_NOT_INIT;
    }
    if (buf == NULL || len == 0U) {
        return SPI_FLASH_ERR_PARAM;
    }
    if (addr > SPI_FLASH_MAX_ADDR || len > (SPI_FLASH_SIZE - addr)) {
        return SPI_FLASH_ERR_PARAM;
    }

    const uint8_t *src = (const uint8_t *)buf;
    spi_flash_os_lock();
    while (len > 0U) {
        uint32_t page_off = addr % SPI_FLASH_PAGE_SIZE;
        uint32_t chunk = SPI_FLASH_PAGE_SIZE - page_off;
        if (chunk > len) {
            chunk = len;
        }
        spi_flash_err_t rc = spi_flash_cmd_page_program4(addr, src, chunk);
        if (rc != SPI_FLASH_OK) {
            spi_flash_os_unlock();
            return rc;
        }
        addr += chunk;
        src  += chunk;
        len  -= chunk;
    }
    spi_flash_os_unlock();
    return SPI_FLASH_OK;
}

spi_flash_err_t spi_flash_erase_sector(uint32_t addr) {
    if (!s_init) {
        return SPI_FLASH_ERR_NOT_INIT;
    }
    if (addr > SPI_FLASH_MAX_ADDR || (addr % SPI_FLASH_SECTOR_SIZE) != 0U) {
        return SPI_FLASH_ERR_PARAM;
    }
    spi_flash_os_lock();
    spi_flash_err_t rc = spi_flash_cmd_erase(CMD_ERASE_4K_4B, addr);
    spi_flash_os_unlock();
    return rc;
}

spi_flash_err_t spi_flash_erase_block_32k(uint32_t addr) {
    if (!s_init) {
        return SPI_FLASH_ERR_NOT_INIT;
    }
    if (addr > SPI_FLASH_MAX_ADDR ||
        (addr % SPI_FLASH_SECTOR_SIZE_32K) != 0U) {
        return SPI_FLASH_ERR_PARAM;
    }
    spi_flash_os_lock();
    spi_flash_err_t rc = spi_flash_cmd_erase(CMD_ERASE_32K_4B, addr);
    spi_flash_os_unlock();
    return rc;
}

spi_flash_err_t spi_flash_erase_block(uint32_t addr) {
    if (!s_init) {
        return SPI_FLASH_ERR_NOT_INIT;
    }
    if (addr > SPI_FLASH_MAX_ADDR || (addr % SPI_FLASH_BLOCK_SIZE) != 0U) {
        return SPI_FLASH_ERR_PARAM;
    }
    spi_flash_os_lock();
    spi_flash_err_t rc = spi_flash_cmd_erase(CMD_ERASE_64K_4B, addr);
    spi_flash_os_unlock();
    return rc;
}

spi_flash_err_t spi_flash_erase_chip(void) {
    if (!s_init) {
        return SPI_FLASH_ERR_NOT_INIT;
    }
    spi_flash_os_lock();
    spi_flash_err_t rc = spi_flash_cmd_chip_erase();
    spi_flash_os_unlock();
    return rc;
}

spi_flash_err_t spi_flash_sync(void) {
    /* Every CS release already commits the current page in m25p80. */
    return s_init ? SPI_FLASH_OK : SPI_FLASH_ERR_NOT_INIT;
}

int spi_flash_selftest(void) {
    uint8_t wbuf[16], rbuf[16];
    spi_flash_info_t info = { 0 };
    spi_flash_err_t rc;

    rc = spi_flash_init(NULL);
    if (rc != SPI_FLASH_OK) {
        printf("spi_flash: init failed (%d)\r\n", (int)rc);
        return -1;
    }
    rc = spi_flash_get_info(&info);
    if (rc != SPI_FLASH_OK) {
        printf("spi_flash: get_info failed (%d)\r\n", (int)rc);
        return -1;
    }
    printf("spi_flash: JEDEC %02X %02X %02X, size=%u MiB, page=%u, "
           "sector=%u, 4B-addr=%d\r\n",
           info.jedec[0], info.jedec[1], info.jedec[2],
           (unsigned)(info.size >> 20), (unsigned)info.page_size,
           (unsigned)info.sector_size, info.four_byte_addr ? 1 : 0);

    /* erase sector 0 */
    rc = spi_flash_erase_sector(0U);
    if (rc != SPI_FLASH_OK) {
        printf("spi_flash: erase sector failed (%d)\r\n", (int)rc);
        return -1;
    }
    /* read-back must be all 0xFF after erase */
    memset(rbuf, 0, sizeof(rbuf));
    spi_flash_read(0U, rbuf, sizeof(rbuf));
    for (unsigned i = 0U; i < sizeof(rbuf); i++) {
        if (rbuf[i] != 0xFFU) {
            printf("spi_flash: erase verify fail @%u (0x%02X)\r\n", i, rbuf[i]);
            return -1;
        }
    }
    /* program a pattern */
    for (unsigned i = 0U; i < sizeof(wbuf); i++) {
        wbuf[i] = (uint8_t)(i * 3U + 1U);
    }
    rc = spi_flash_write(0U, wbuf, sizeof(wbuf));
    if (rc != SPI_FLASH_OK) {
        printf("spi_flash: write failed (%d)\r\n", (int)rc);
        return -1;
    }
    /* read back and compare */
    memset(rbuf, 0, sizeof(rbuf));
    spi_flash_read(0U, rbuf, sizeof(rbuf));
    for (unsigned i = 0U; i < sizeof(wbuf); i++) {
        if (rbuf[i] != wbuf[i]) {
            printf("spi_flash: verify fail @%u (0x%02X != 0x%02X)\r\n",
                   i, rbuf[i], wbuf[i]);
            return -1;
        }
    }
    printf("spi_flash: selftest OK (pattern on sector 0)\r\n");
    return 0;
}
