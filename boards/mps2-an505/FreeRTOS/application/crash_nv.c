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

/* Print a stored crash record at boot (phase 3 will archive it to a file and
 * mark it consumed).  Silent when nothing is stored. */
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
}
