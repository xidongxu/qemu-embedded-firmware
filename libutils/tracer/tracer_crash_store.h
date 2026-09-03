/*
 * tracer_crash_store.h - media-agnostic crash-record store (slot strategy).
 *
 * The "staging" half of the two-stage crash record (see tracer.h
 * TRACER_USE_CRASH): once tracer has assembled a crash record (dump text
 * + pre-crash ring + CRC footer) it hands it to the weak tracer_crash_save()
 * hook.  This file implements that hook as a MEDIA-AGNOSTIC store:
 *
 *   - a reserved non-volatile region is split into N slots (typically 2);
 *   - each slot = [16 B header: magic 'TNC1' | len | crc32][payload];
 *   - a crash is written to the slot that is NOT currently valid (alternate),
 *     erase slot -> write header -> write payload; a power loss in the
 *     middle leaves that slot failing its CRC check at the next boot, and
 *     the OTHER slot's previous record is still readable;
 *   - tracer_crash_store_read_latest() returns the newest valid record
 *     (magic + payload CRC both checked), tracer_crash_store_clear() erases
 *     the current one so it is not re-reported after archiving.
 *
 * All MEDIA-SPECIFIC work is behind the weak primitives declared below
 * (get_media / erase / write / read), which each board provides -- e.g.
 * mps2 wraps spi_flash_* over the SPI NOR top, stm32 would wrap the HAL
 * internal-flash driver.  Without a board-provided media, get_media() fails
 * and every operation is a silent no-op, so a build that never enables the
 * crash store needs no board glue at all.
 *
 * The file tracer_crash_store.c is compiled into the tracer library when
 * TRACER_USE_CRASH is enabled (it only contains code under that switch).
 */
#ifndef TRACER_CRASH_STORE_H
#define TRACER_CRASH_STORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Media geometry / attributes reported by the board. */
typedef struct {
    /* Reserved region start address (must be aligned to slot_size). */
    uint32_t slot_base;
    /* One slot = one erase unit, in bytes (e.g. 4 KiB SPI NOR sector, or an
     * internal-flash sector on stm32). */
    uint32_t slot_size;
    /* Number of slots (>= 2 recommended so a half-written crash never
     * destroys the previous record). */
    uint32_t slot_count;
} tracer_crash_store_media_t;

/* ---- Media primitives: implement these on the board (weak symbols) -------
 *
 * get_media: fill @info with the reserved-region geometry.  Return non-zero
 *            when this build has NO crash store media (store is silent).
 * erase:     erase ONE slot (address is slot-aligned; a slot's erase unit).
 * write:     write @len bytes to @addr (the region must already be erased,
 *            NOR semantics); may split across pages internally.
 * read:      read @len bytes from @addr.
 * All return 0 on success, non-zero on error.
 */
int tracer_crash_store_get_media(tracer_crash_store_media_t *info);
int tracer_crash_store_erase(uint32_t addr);
int tracer_crash_store_write(uint32_t addr, const void *buf, uint32_t len);
int tracer_crash_store_read(uint32_t addr, void *buf, uint32_t len);

/* ---- Store strategy API (implemented in tracer_crash_store.c) ------------
 *
 * tracer_crash_save() is the STRONG implementation of tracer's weak hook
 * (tracer.h).  Called from the fault/assert path right before trapping /
 * resetting, with IRQs masked.  It writes @record into the non-current slot.
 * If no media is configured it returns silently.
 *
 * tracer_crash_store_read_latest(): copy the newest valid record into @buf
 * (up to @cap bytes) and return its length; 0 when nothing is stored.
 *
 * tracer_crash_store_clear(): erase the current valid record (call after it
 * has been archived, so the next boot does not re-report it).
 */
void tracer_crash_save(const void *record, uint32_t len);
uint32_t tracer_crash_store_read_latest(void *buf, uint32_t cap);
void tracer_crash_store_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* TRACER_CRASH_STORE_H */
