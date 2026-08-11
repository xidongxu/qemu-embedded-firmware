/*
 * lfs_port_spi_flash.c - littlefs block-device backend on the SPI NOR flash.
 *
 * Maps littlefs block operations onto the spi_flash_* driver:
 *   read / prog  -> byte-addressed spi_flash_read() / spi_flash_write()
 *   erase        -> 4 KiB sector erase (block_size == flash sector)
 *   sync         -> spi_flash_sync()
 *
 * With LFS_THREADSAFE defined, lock/unlock map to the driver's
 * spi_flash_os_lock()/spi_flash_os_unlock() weak hooks (override them with a
 * FreeRTOS mutex etc. to make the filesystem multi-task safe).
 */
#include "lfs_port_spi_flash.h"

#include <stdbool.h>
#include <string.h>

/* ---- single-instance static buffers (see header note) ---- */
static uint8_t s_read_buf[SPI_FLASH_PAGE_SIZE];
static uint8_t s_prog_buf[SPI_FLASH_PAGE_SIZE];
static uint8_t s_lookahead_buf[32];

/* ---- SPI flash lazy init ---- */
static bool s_flash_ready;

static void lfs_port_flash_ensure_init(void)
{
    if (!s_flash_ready && spi_flash_init(NULL) == SPI_FLASH_OK) {
        s_flash_ready = true;
    }
}

/* ---- lfs_config block-device callbacks ---- */

static int lfs_port_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                         void *buffer, lfs_size_t size)
{
    uint32_t addr = (uint32_t)block * c->block_size + (uint32_t)off;
    return (spi_flash_read(addr, buffer, (uint32_t)size) == SPI_FLASH_OK)
               ? LFS_ERR_OK
               : LFS_ERR_IO;
}

static int lfs_port_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                         const void *buffer, lfs_size_t size)
{
    uint32_t addr = (uint32_t)block * c->block_size + (uint32_t)off;
    return (spi_flash_write(addr, buffer, (uint32_t)size) == SPI_FLASH_OK)
               ? LFS_ERR_OK
               : LFS_ERR_IO;
}

static int lfs_port_erase(const struct lfs_config *c, lfs_block_t block)
{
    uint32_t addr = (uint32_t)block * c->block_size;
    return (spi_flash_erase_sector(addr) == SPI_FLASH_OK) ? LFS_ERR_OK
                                                           : LFS_ERR_IO;
}

static int lfs_port_sync(const struct lfs_config *c)
{
    (void)c;
    return (spi_flash_sync() == SPI_FLASH_OK) ? LFS_ERR_OK : LFS_ERR_IO;
}

#ifdef LFS_THREADSAFE
static int lfs_port_lock(const struct lfs_config *c)
{
    (void)c;
    spi_flash_os_lock();
    return LFS_ERR_OK;
}

static int lfs_port_unlock(const struct lfs_config *c)
{
    (void)c;
    spi_flash_os_unlock();
    return LFS_ERR_OK;
}
#endif

/* ---- public API ---- */

void lfs_spi_flash_config_init(struct lfs_config *cfg)
{
    lfs_port_flash_ensure_init();

    memset(cfg, 0, sizeof(*cfg));
    cfg->read   = lfs_port_read;
    cfg->prog   = lfs_port_prog;
    cfg->erase  = lfs_port_erase;
    cfg->sync   = lfs_port_sync;
#ifdef LFS_THREADSAFE
    cfg->lock   = lfs_port_lock;
    cfg->unlock = lfs_port_unlock;
#endif

    cfg->read_size      = SPI_FLASH_PAGE_SIZE;
    cfg->prog_size      = SPI_FLASH_PAGE_SIZE;
    cfg->block_size     = LFS_PORT_SPI_FLASH_BLOCK_SIZE;
    cfg->block_count    = LFS_PORT_SPI_FLASH_BLOCK_COUNT;
    cfg->block_cycles   = 100;
    cfg->cache_size     = SPI_FLASH_PAGE_SIZE;
    cfg->lookahead_size = sizeof(s_lookahead_buf);

    cfg->read_buffer      = s_read_buf;
    cfg->prog_buffer      = s_prog_buf;
    cfg->lookahead_buffer = s_lookahead_buf;
}

int lfs_spi_flash_mount(lfs_t *lfs, struct lfs_config *cfg, int format_if_invalid)
{
    lfs_port_flash_ensure_init();

    int err = lfs_mount(lfs, cfg);
    if (err == LFS_ERR_OK || !format_if_invalid) {
        return err;
    }

    err = lfs_format(lfs, cfg);
    if (err != LFS_ERR_OK) {
        return err;
    }
    return lfs_mount(lfs, cfg);
}

int lfs_spi_flash_format(lfs_t *lfs, struct lfs_config *cfg)
{
    lfs_port_flash_ensure_init();
    return lfs_format(lfs, cfg);
}
