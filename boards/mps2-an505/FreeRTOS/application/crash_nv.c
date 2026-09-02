/*
 * crash_nv.c - non-volatile crash-record staging (mps2-an505 FreeRTOS).
 *
 * Overrides the weak tracer_crash_save() (tracer.h / TRACER_USE_CRASHLOG)
 * and stores the crash record into a reserved 2 x 4 KiB area at the top of
 * the external SPI NOR (w25q02jvm, 256 MiB):
 *
 *     CRASH_NV_BASE = 0x0FFE0000            slot A
 *     CRASH_NV_BASE + 0x1000                slot B
 *
 * Slot layout (each slot is one 4 KiB NOR sector):
 *
 *     [0..15]    header : magic 'TNC1' | payload len | payload crc32
 *     [16..]     payload: the tracer crash record text (dump + ring + CRC)
 *
 * Write sequence (defensive: a power loss at any point must not corrupt
 * boot, and should leave the previous record intact):
 *     1. pick the slot that is NOT currently valid (alternate A/B)
 *     2. erase that sector (NOR semantics: write only turns 1 -> 0)
 *     3. write the header, then the payload
 * Boot side accepts a slot only if the magic matches and
 * crc32(payload) == header.crc.  An interrupted write fails that check and
 * is ignored; the other slot (if valid) is still reported.
 *
 * The SPI-flash driver is initialized lazily (once) on first use, so this
 * works even though the PJ_PHONE build skips the normal fatfs/spi_flash
 * self-test in main.c (and that build never formats the NOR, so this
 * reserved area never overlaps a filesystem volume).
 */
#include <stdio.h>

#include "crash_nv.h"
#include "lfs.h"
#include "lfs_port_spi_flash.h"
#include "spi_flash.h"
#include "uart.h"

#define CRASH_NV_MAGIC   0x31434E54u  /* 'TNC1' */
#define CRASH_NV_BASE    0x0FFE0000u
#define CRASH_NV_SLOT    SPI_FLASH_SECTOR_SIZE  /* 4096 = one 4 KiB sector */
#define CRASH_NV_SLOTS   2u
#define CRASH_NV_HDR     16u
#define CRASH_NV_MAX     (CRASH_NV_SLOT - CRASH_NV_HDR)

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint32_t crc;
    uint32_t rsvd;
} crash_nv_hdr_t;

static int s_nv_ready = 0;

/* Single-writer scratch used to validate a slot (kept static, not on the
 * fault-path stack). */
static uint8_t s_scratch[CRASH_NV_MAX];

/* CRC-32 (IEEE 802.3, poly 0xEDB88320, reflected, bitwise -- no table). */
static uint32_t crash_nv_crc32(const uint8_t *p, uint32_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n-- != 0u) {
        crc ^= (uint32_t)*p++;
        for (unsigned i = 0u; i < 8u; i++) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}

/* Lazily bring up the SPI-flash driver (idempotent). */
static void crash_nv_ensure(void) {
    if (!s_nv_ready) {
        spi_flash_init(NULL);
        s_nv_ready = 1;
    }
}

/* Read and validate slot @slot.  If valid, copies the payload into @buf
 * (@cap bytes max) and returns its length; otherwise returns 0. */
static uint32_t crash_nv_slot_read(uint32_t slot, uint8_t *buf, uint32_t cap) {
    crash_nv_hdr_t hdr;
    uint32_t addr = CRASH_NV_BASE + slot * CRASH_NV_SLOT;

    if (spi_flash_read(addr, &hdr, sizeof(hdr)) != SPI_FLASH_OK) {
        return 0u;
    }
    if (hdr.magic != CRASH_NV_MAGIC || hdr.len == 0u ||
        hdr.len > CRASH_NV_MAX || hdr.len > cap) {
        return 0u;
    }
    if (spi_flash_read(addr + CRASH_NV_HDR, buf, hdr.len) != SPI_FLASH_OK) {
        return 0u;
    }
    if (crash_nv_crc32(buf, hdr.len) != hdr.crc) {
        return 0u;
    }
    return hdr.len;
}

/* Index of the currently valid slot, or -1 if none. */
static int crash_nv_current(void) {
    uint32_t s;
    for (s = 0u; s < CRASH_NV_SLOTS; s++) {
        if (crash_nv_slot_read(s, s_scratch, (uint32_t)sizeof(s_scratch)) != 0u) {
            return (int)s;
        }
    }
    return -1;
}

/* Non-volatile write hook: store a finished crash record.  Called by tracer
 * from the fault/assert path right before it traps or resets.  IRQs are
 * already masked there (cpsid i in the asm entry), so this is a safe
 * single-writer: no other task can be mid-transaction on the SPI flash. */
void tracer_crash_save(const void *data, uint32_t len) {
    crash_nv_hdr_t hdr;
    uint32_t slot;
    uint32_t addr;
    int cur;

    if (data == NULL || len == 0u || len > CRASH_NV_MAX) {
        return;
    }
    crash_nv_ensure();

    /* Alternate: write into the slot that is not currently valid. */
    cur = crash_nv_current();
    if (cur == 0) {
        slot = 1u;
    } else if (cur == 1) {
        slot = 0u;
    } else {
        slot = 0u;
    }
    addr = CRASH_NV_BASE + slot * CRASH_NV_SLOT;

    hdr.magic = CRASH_NV_MAGIC;
    hdr.len = len;
    hdr.crc = crash_nv_crc32((const uint8_t *)data, len);
    hdr.rsvd = 0u;

    if (spi_flash_erase_sector(addr) != SPI_FLASH_OK) {
        return;
    }
    if (spi_flash_write(addr, &hdr, sizeof(hdr)) != SPI_FLASH_OK) {
        return;
    }
    (void)spi_flash_write(addr + CRASH_NV_HDR, data, len);
}

/* ---- boot-side read back ------------------------------------------------- */

uint32_t crash_nv_read_latest(uint8_t *buf, uint32_t cap) {
    int32_t s;
    uint32_t len;

    crash_nv_ensure();
    for (s = (int32_t)CRASH_NV_SLOTS - 1; s >= 0; s--) {
        len = crash_nv_slot_read((uint32_t)s, buf, cap);
        if (len != 0u) {
            return len;
        }
    }
    return 0u;
}

/* ===== littlefs archive (phase 3) ========================================
 *
 * After a crash the staging record is moved into a real file on a small
 * littlefs volume that lives BELOW the crash staging area (so the two never
 * overlap):
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
static uint8_t s_old[CRASH_NV_MAX];  /* previous crash_last.txt content */

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

/* Erase the stored crash record (mark it consumed) so the next boot does not
 * re-report it.  Called only after the archive has succeeded. */
static void crash_nv_clear(void) {
    int cur = crash_nv_current();
    if (cur >= 0) {
        (void)spi_flash_erase_sector(CRASH_NV_BASE +
                                     (uint32_t)cur * CRASH_NV_SLOT);
    }
}

/* Boot-time handling of a stored crash record: print it, archive it into
 * littlefs files, then clear the staging record (no duplicate report next
 * boot).  Silent when nothing is stored. */
void crash_nv_boot_report(void) {
    static uint8_t s_rec[CRASH_NV_MAX];
    uint32_t len = crash_nv_read_latest(s_rec, (uint32_t)sizeof(s_rec));
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
        crash_nv_clear();
        printf("[crash] archived to littlefs crash_last.txt (%lu bytes); "
               "staging cleared\r\n", (unsigned long)len);
    } else {
        printf("[crash] archive FAILED - record kept in staging\r\n");
    }
}
