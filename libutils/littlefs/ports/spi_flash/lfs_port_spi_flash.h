/*
 * lfs_port_spi_flash.h - littlefs block-device backend on the SPI NOR flash.
 *
 * Implements the lfs_config callbacks (read/prog/erase/sync) on top of the
 * on-board SPI flash driver (spi_flash.h).  The whole 256 MiB device is
 * exposed as one littlefs volume with 4 KiB blocks (= the flash sector).
 *
 * Typical usage (single volume):
 *
 *     lfs_t       lfs;
 *     lfs_config  cfg;
 *
 *     lfs_spi_flash_config_init(&cfg);
 *     if (lfs_spi_flash_mount(&lfs, &cfg, 1) != LFS_ERR_OK) {
 *         // mount failed (and auto-format was disabled or also failed)
 *     }
 *
 *     // ... lfs_file_open() / lfs_file_write() ...
 *
 *     lfs_unmount(&lfs);
 *
 * The SPI flash driver is probed/initialised automatically on the first
 * lfs_spi_flash_mount()/lfs_spi_flash_format() call, so there is no need to
 * call spi_flash_init() beforehand (though doing so is harmless).
 *
 * NOTE: the port uses file-scope static buffers, so it supports a single
 * mounted littlefs instance.  This is fine for typical embedded use.
 */
#ifndef LFS_PORT_SPI_FLASH_H
#define LFS_PORT_SPI_FLASH_H

#include "lfs.h"
#include "spi_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* littlefs geometry backed by the SPI NOR flash. */
#define LFS_PORT_SPI_FLASH_BLOCK_SIZE    SPI_FLASH_SECTOR_SIZE
#define LFS_PORT_SPI_FLASH_BLOCK_COUNT   (SPI_FLASH_SIZE / SPI_FLASH_SECTOR_SIZE)

/*
 * Fill @cfg with the block-device callbacks and geometry.  @cfg stays owned
 * by the caller and must remain valid while the filesystem is mounted.
 */
void lfs_spi_flash_config_init(struct lfs_config *cfg);

/*
 * Mount the volume.  If the on-disk superblock is invalid and
 * @format_if_invalid is non-zero, the device is formatted first, then
 * mounted again.  Returns LFS_ERR_OK on success.
 */
int lfs_spi_flash_mount(lfs_t *lfs, struct lfs_config *cfg, int format_if_invalid);

/*
 * Format the whole device with a fresh littlefs filesystem.
 * WARNING: erases the entire flash.  Returns LFS_ERR_OK on success.
 */
int lfs_spi_flash_format(lfs_t *lfs, struct lfs_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* LFS_PORT_SPI_FLASH_H */
