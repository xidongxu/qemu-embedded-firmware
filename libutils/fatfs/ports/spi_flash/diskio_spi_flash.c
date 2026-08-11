/*
 * diskio_spi_flash.c - FatFs low-level disk I/O on the SPI NOR flash.
 *
 * Implements the FatFs disk_* interface (diskio.h) on top of the on-board
 * SPI flash driver (spi_flash.h).  Replaces the upstream template
 * source/diskio.c, which is not compiled (see libutils/fatfs/CMakeLists.txt).
 */
#include "diskio_spi_flash.h"
#include "diskio.h"

#include <stdbool.h>

/* Map a 512-byte LBA to a flash byte address. */
static uint32_t diskio_lba_to_addr(LBA_t sector)
{
    return (uint32_t)(sector * DISKIO_SPI_FLASH_SECTOR_SIZE);
}

static bool s_ready;

int diskio_spi_flash_init(void)
{
    if (s_ready) {
        return 0;
    }
    if (spi_flash_init(NULL) == SPI_FLASH_OK) {
        s_ready = true;
        return 0;
    }
    return -1;
}

/* Get Disk Status */
DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != DISKIO_SPI_FLASH_PDRV) {
        return STA_NOINIT;
    }
    return s_ready ? 0 : STA_NOINIT;
}

/* Initialize a Drive */
DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != DISKIO_SPI_FLASH_PDRV) {
        return STA_NOINIT;
    }
    s_ready = (diskio_spi_flash_init() == 0);
    return s_ready ? 0 : STA_NOINIT;
}

/* Read Sector(s) */
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != DISKIO_SPI_FLASH_PDRV) {
        return RES_PARERR;
    }
    if (!s_ready) {
        return RES_NOTRDY;
    }
    uint32_t len = (uint32_t)count * DISKIO_SPI_FLASH_SECTOR_SIZE;
    if (spi_flash_read(diskio_lba_to_addr(sector), buff, len) != SPI_FLASH_OK) {
        return RES_ERROR;
    }
    return RES_OK;
}

/* Write Sector(s) */
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != DISKIO_SPI_FLASH_PDRV) {
        return RES_PARERR;
    }
    if (!s_ready) {
        return RES_NOTRDY;
    }
    uint32_t len = (uint32_t)count * DISKIO_SPI_FLASH_SECTOR_SIZE;
    if (spi_flash_write(diskio_lba_to_addr(sector), buff, len) != SPI_FLASH_OK) {
        return RES_ERROR;
    }
    return RES_OK;
}

/*
 * Trim (discard) a range of sectors: erase the overlapping 4 KiB flash
 * sectors.  This is a hint, so erasing a bit more than asked is allowed.
 */
static DRESULT diskio_trim(const LBA_t *range)
{
    uint32_t start = diskio_lba_to_addr(range[0]);
    uint32_t end   = diskio_lba_to_addr(range[0] + range[1]);

    uint32_t cur = start & ~(DISKIO_SPI_FLASH_BLOCK_SIZE - 1U);
    for (; cur < end; cur += DISKIO_SPI_FLASH_BLOCK_SIZE) {
        if (spi_flash_erase_sector(cur) != SPI_FLASH_OK) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

/* Miscellaneous Functions */
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != DISKIO_SPI_FLASH_PDRV) {
        return RES_PARERR;
    }
    if (!s_ready) {
        return RES_NOTRDY;
    }

    switch (cmd) {
    case CTRL_SYNC:
        return (spi_flash_sync() == SPI_FLASH_OK) ? RES_OK : RES_ERROR;

    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = (LBA_t)DISKIO_SPI_FLASH_SECTOR_COUNT;
        return RES_OK;

    case GET_SECTOR_SIZE:
        *(WORD *)buff = (WORD)DISKIO_SPI_FLASH_SECTOR_SIZE;
        return RES_OK;

    case GET_BLOCK_SIZE:
        /* Erase block size expressed in logical sectors (4096/512 = 8). */
        *(DWORD *)buff = DISKIO_SPI_FLASH_BLOCK_SIZE / DISKIO_SPI_FLASH_SECTOR_SIZE;
        return RES_OK;

#if FF_USE_TRIM
    case CTRL_TRIM:
        return diskio_trim((const LBA_t *)buff);
#endif

    default:
        return RES_PARERR;
    }
}
