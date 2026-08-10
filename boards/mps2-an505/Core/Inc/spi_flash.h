/*
 * spi_flash.h - industrial SPI NOR flash driver for the mps2-an505 (QEMU).
 *
 * Targets the Winbond w25q02jvm (256 MiB) behind PL022 SSP0, with the flash
 * chip-select line driven by bit 8 of the MPS2 FPGAIO MISC register.
 *
 * The API is filesystem-friendly (LittleFS / FatFS style): byte-addressed
 * read/write/erase with automatic page splitting, a probe/identify step,
 * explicit error codes and busy-polling with timeout.  All operations are
 * re-entrant at the "single consumer" level; optional OS lock hooks make the
 * driver multi-task safe (see spi_flash_os_lock()/spi_flash_os_unlock()).
 *
 * LittleFS glue sketch (block device == whole flash, block_size == sector):
 *     static int lfs_read(const lfs_config *c, lfs_block_t b, lfs_off_t o,
 *                         void *buf, lfs_size_t s) {
 *         return spi_flash_read(b * SPI_FLASH_SECTOR_SIZE + o, buf, s);
 *     }
 *     static int lfs_prog(const lfs_config *c, lfs_block_t b, lfs_off_t o,
 *                         const void *buf, lfs_size_t s) {
 *         return spi_flash_write(b * SPI_FLASH_SECTOR_SIZE + o, buf, s);
 *     }
 *     static int lfs_erase(const lfs_config *c, lfs_block_t b) {
 *         return spi_flash_erase_sector(b * SPI_FLASH_SECTOR_SIZE);
 *     }
 */
#ifndef SPI_FLASH_H
#define SPI_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* page-program size */
#define SPI_FLASH_PAGE_SIZE          (256U)
/* 4 KiB sector */
#define SPI_FLASH_SECTOR_SIZE        (4096U)
/* 32 KiB block */
#define SPI_FLASH_SECTOR_SIZE_32K    (32768U)
/* 64 KiB block */
#define SPI_FLASH_BLOCK_SIZE         (65536U)
/* 256 MiB total */
#define SPI_FLASH_SIZE               (0x10000000U)
#define SPI_FLASH_MAX_ADDR           (SPI_FLASH_SIZE - 1U)

/* expected JEDEC ID for the w25q02jvm */
#define SPI_FLASH_JEDEC_MFR          (0xEFU)
#define SPI_FLASH_JEDEC_TYPE         (0x70U)
#define SPI_FLASH_JEDEC_CAP          (0x22U)

typedef enum {
    SPI_FLASH_OK = 0,
    /* bad argument / out-of-range address */
    SPI_FLASH_ERR_PARAM,
    /* SPI transfer failure */
    SPI_FLASH_ERR_IO,
    /* device busy timeout */
    SPI_FLASH_ERR_TIMEOUT,
    /* JEDEC probe failed / wrong part */
    SPI_FLASH_ERR_PROBE,
    /* driver not initialised */
    SPI_FLASH_ERR_NOT_INIT,
    /* write-enable or program failed */
    SPI_FLASH_ERR_WRITE,
} spi_flash_err_t;

/* probe / geometry info returned by spi_flash_init() */
typedef struct {
    /* manufacturer / type / capacity bytes */
    uint8_t  jedec[3];
    /* total size in bytes */
    uint32_t size;
    /* program page size */
    uint16_t page_size;
    /* smallest erase unit (4 KiB) */
    uint32_t sector_size;
    /* 64 KiB erase unit */
    uint32_t block_size;
    /* driver uses 4-byte address commands */
    bool     four_byte_addr;
} spi_flash_info_t;

/* configuration for spi_flash_init(); NULL selects defaults */
typedef struct {
    /* busy-poll timeout for slow ops (ms) */
    uint32_t poll_timeout_ms;
} spi_flash_config_t;

/*
 * Initialise the driver and probe the device.  @cfg may be NULL to use
 * defaults.  Returns SPI_FLASH_ERR_PROBE if the JEDEC ID does not match
 * the expected part.
 */
spi_flash_err_t spi_flash_init(const spi_flash_config_t *cfg);

/* De-initialise the driver (releases chip-select, idles the SPI). */
spi_flash_err_t spi_flash_deinit(void);

/* Return the probed device information. */
spi_flash_err_t spi_flash_get_info(spi_flash_info_t *info);

/* Read @len bytes from @addr into @buf.  Any length is supported. */
spi_flash_err_t spi_flash_read(uint32_t addr, void *buf, uint32_t len);

/*
 * Write @len bytes from @buf to @addr.  Automatically splits across page
 * boundaries; the target region must already be erased (NOR semantics).
 * Data is committed when the transaction chip-select is released, so no
 * separate flush is required.
 */
spi_flash_err_t spi_flash_write(uint32_t addr, const void *buf, uint32_t len);

/* Erase a 4 KiB sector (@addr must be 4 KiB aligned). */
spi_flash_err_t spi_flash_erase_sector(uint32_t addr);

/* Erase a 32 KiB block (@addr must be 32 KiB aligned). */
spi_flash_err_t spi_flash_erase_block_32k(uint32_t addr);

/* Erase a 64 KiB block (@addr must be 64 KiB aligned). */
spi_flash_err_t spi_flash_erase_block(uint32_t addr);

/* Erase the whole device.  Slow; use sparingly. */
spi_flash_err_t spi_flash_erase_chip(void);

/*
 * Flush any pending writes to the backing store.  In QEMU (m25p80) every
 * chip-select release already commits the current page, so this is a no-op
 * kept for API completeness / portability to real hardware.
 */
spi_flash_err_t spi_flash_sync(void);

/*
 * Optional OS lock hooks.  Weak no-op symbols by default.  To make the
 * driver multi-task safe, define SPI_FLASH_USE_OS_LOCK and provide these,
 * or override the weak symbols (e.g. wrap a FreeRTOS mutex).
 */
void spi_flash_os_lock(void);
void spi_flash_os_unlock(void);

/*
 * Self-test: probe + erase/program/read-back on sector 0.
 * NOTE: erases sector 0 of the device.  Returns 0 on success.
 */
int spi_flash_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* SPI_FLASH_H */
