/*
 * crash_nv.c - mps2-an505 SPI NOR media + littlefs archive for the crash log.
 *
 * Media glue for tracer_crash_store (libutils/tracer/tracer_crash_store.c):
 * this file implements the weak media primitives (get_media / erase / write /
 * read) over the external SPI NOR (w25q02jvm) reserved top area, and
 * provides the boot-side archive of a stored crash record into littlefs
 * files.
 *
 * Reserved top-of-NOR crash slots: 0x0FFE0000, 2 x 4 KiB (slot A / B).  ALL
 * slot-strategy logic (alternating slots, slot header + CRC, anti-partial-
 * write ordering, read-latest, clear) lives in the media-agnostic
 * tracer_crash_store.c -- a different board only needs its own media glue.
 *
 * The SPI-flash driver is lazily initialised on first media use, so this
 * works even though the PJ_PHONE build skips the normal fatfs/spi_flash
 * self-test in main.c (and that build never formats the NOR, so the reserved
 * area never overlaps a filesystem volume).
 */
#include <stdio.h>

#include "crash_nv.h"
#include "lfs.h"
#include "lfs_port_spi_flash.h"
#include "spi_flash.h"
#include "tracer_crash_store.h"
#include "uart.h"

#define CRASH_NV_BASE  0x0FFE0000u
#define CRASH_NV_SLOT  SPI_FLASH_SECTOR_SIZE       /* 4096 = one 4 KiB sector */
#define CRASH_NV_SLOTS 2u
/* Maximum record that fits one slot (slot header is 16 B). */
#define CRASH_NV_MAX   (CRASH_NV_SLOT - 16u)

static int s_nv_ready = 0;

static void crash_nv_ensure(void) {
    if (!s_nv_ready) {
        spi_flash_init(NULL);
        s_nv_ready = 1;
    }
}

/* ===== tracer_crash_store media primitives (override the weak defaults) == */

int tracer_crash_store_get_media(tracer_crash_store_media_t *info) {
    crash_nv_ensure();
    if (info == NULL) {
        return -1;
    }
    info->slot_base = CRASH_NV_BASE;
    info->slot_size = CRASH_NV_SLOT;
    info->slot_count = CRASH_NV_SLOTS;
    return 0;
}

int tracer_crash_store_erase(uint32_t addr) {
    return (spi_flash_erase_sector(addr) == SPI_FLASH_OK) ? 0 : -1;
}

int tracer_crash_store_write(uint32_t addr, const void *buf, uint32_t len) {
    return (spi_flash_write(addr, buf, len) == SPI_FLASH_OK) ? 0 : -1;
}

int tracer_crash_store_read(uint32_t addr, void *buf, uint32_t len) {
    return (spi_flash_read(addr, buf, len) == SPI_FLASH_OK) ? 0 : -1;
}

/* ===== boot-side archive into littlefs ====================================
 *
 * After a crash the staging record (read via tracer_crash_store_read_latest)
 * is moved into a real file on a small littlefs volume that lives BELOW the
 * crash staging area (so the two never overlap):
 *
 *     [0                  ... CRASH_NV_BASE)  littlefs volume
 *     [CRASH_NV_BASE ...  flash top)          crash staging slots (2x4 KiB)
 *
 * lfs_spi_flash_config_init() defaults to the whole 256 MiB as one volume,
 * so we shrink cfg.block_count to CRASH_NV_BASE before mounting -- the crash
 * slots at the top are never touched by the filesystem.
 *
 * The volume is only mounted when there is a record to archive (i.e. once,
 * on the first boot after a crash); on a normal boot with nothing stored,
 * no filesystem work happens at all (keeps the PJ_PHONE build free of FS
 * overhead in the common case).
 */
static lfs_t s_lfs;
static struct lfs_config s_lfs_cfg;
static uint8_t s_rec[CRASH_NV_MAX];   /* newest crash record (from staging) */
static uint8_t s_old[CRASH_NV_MAX];   /* previous crash_last.txt content */

/* littlefs volume = everything below the crash staging area. */
#define CRASH_NV_LFS_BLOCKS (CRASH_NV_BASE / SPI_FLASH_SECTOR_SIZE)

/* Move the stored crash record into littlefs files:
 *   crash_last.txt  = newest crash record
 *   crash_prev.txt  = previous one (rolled)
 * Returns 0 on success. */
static int crash_nv_archive_to_fs(const uint8_t *rec, uint32_t len) {
    lfs_file_t f;
    int r;

    if (rec == NULL || len == 0u) {
        return -1;
    }
    lfs_spi_flash_config_init(&s_lfs_cfg);
    s_lfs_cfg.block_count = CRASH_NV_LFS_BLOCKS; /* never touch crash slots */
    r = lfs_spi_flash_mount(&s_lfs, &s_lfs_cfg, 1);
    if (r != LFS_ERR_OK) {
        return -1;
    }

    /* Roll crash_prev.txt <- crash_last.txt. */
    r = lfs_file_open(&s_lfs, &f, "crash_last.txt", LFS_O_RDONLY);
    if (r == LFS_ERR_OK) {
        lfs_ssize_t n = lfs_file_read(&s_lfs, &f, s_old, (lfs_size_t)sizeof(s_old));
        lfs_file_close(&s_lfs, &f);
        if (n > 0) {
            r = lfs_file_open(&s_lfs, &f, "crash_prev.txt",
                              LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
            if (r == LFS_ERR_OK) {
                (void)lfs_file_write(&s_lfs, &f, s_old, (lfs_size_t)n);
                lfs_file_close(&s_lfs, &f);
            }
        }
    }

    /* Write the new record to crash_last.txt. */
    r = lfs_file_open(&s_lfs, &f, "crash_last.txt",
                      LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (r != LFS_ERR_OK) {
        lfs_unmount(&s_lfs);
        return -1;
    }
    (void)lfs_file_write(&s_lfs, &f, rec, (lfs_size_t)len);
    lfs_file_close(&s_lfs, &f);
    lfs_unmount(&s_lfs);
    return 0;
}

/* Boot-time handling of a stored crash record: print it, archive it into
 * littlefs files, then clear the staging record via the store (no duplicate
 * report next boot).  Silent when nothing is stored. */
void crash_nv_boot_report(void) {
    uint32_t len = tracer_crash_store_read_latest(s_rec, (uint32_t)sizeof(s_rec));
    uint32_t i;

    if (len == 0u) {
        return;
    }
    printf("\r\n===== Crash record from last reset =====\r\n");
    for (i = 0u; i < len; i++) {
        put_char((char)s_rec[i]);
    }
    printf("\r\n===== End of stored crash record =====\r\n");

    if (crash_nv_archive_to_fs(s_rec, len) == 0) {
        tracer_crash_store_clear();
        printf("[crash] archived to littlefs crash_last.txt (%lu bytes); "
               "staging cleared\r\n", (unsigned long)len);
    } else {
        printf("[crash] archive FAILED - record kept in staging\r\n");
    }
}
