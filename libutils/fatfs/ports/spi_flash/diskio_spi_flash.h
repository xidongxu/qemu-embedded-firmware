/*
 * diskio_spi_flash.h - FatFs low-level disk backend on the SPI NOR flash.
 *
 * Exposes the whole 256 MiB SPI NOR flash as a logical block device with
 * 512-byte sectors (the smallest sector size FatFs supports; the flash
 * driver is byte-addressable, so 512-byte reads/writes map straight onto
 * it).  The disk_* entry points required by FatFs are implemented in
 * diskio_spi_flash.c.
 *
 * To attach the volume in application code:
 *
 *     FATFS fs;
 *     if (f_mount(&fs, "", 1) != FR_OK) { ... }   // auto-inits the disk
 *
 * To (re)format the volume (erases the whole flash):
 *
 *     FATFS  fs;
 *     BYTE   work[FF_MAX_SS];
 *     f_mount(&fs, "", 0);
 *     f_mkfs("", FM_FAT, 0, work, sizeof(work));
 */
#ifndef DISKIO_SPI_FLASH_H
#define DISKIO_SPI_FLASH_H

#include "ff.h"
#include "spi_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Logical sector size exposed to FatFs. */
#define DISKIO_SPI_FLASH_SECTOR_SIZE    512U
/* Total number of logical sectors (256 MiB / 512). */
#define DISKIO_SPI_FLASH_SECTOR_COUNT   (SPI_FLASH_SIZE / DISKIO_SPI_FLASH_SECTOR_SIZE)
/* Flash erase granularity (4 KiB sector = 8 logical sectors). */
#define DISKIO_SPI_FLASH_BLOCK_SIZE     SPI_FLASH_SECTOR_SIZE
/* Physical drive number used by this port. */
#define DISKIO_SPI_FLASH_PDRV           0U

/*
 * Probe/initialise the SPI flash once.  Called lazily by disk_initialize();
 * returns 0 on success.  Applications may call it directly to fail fast.
 */
int diskio_spi_flash_init(void);

#ifdef __cplusplus
}
#endif

#endif /* DISKIO_SPI_FLASH_H */
