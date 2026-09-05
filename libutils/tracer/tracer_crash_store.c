/*
 * tracer_crash_store.c - media-agnostic crash-record store (see header).
 *
 * Compiled into the tracer library only when TRACER_USE_CRASH is enabled
 * (the whole file is guarded by that switch).  Provides the STRONG
 * implementation of tracer's weak tracer_crash_save() hook plus the read /
 * clear helpers, on top of the board-provided weak media primitives
 * (tracer_crash_store_get_media/erase/write/read).
 *
 * Slot layout (one slot = one erase unit of the media):
 *     [0..15]   header: magic 'TNC1' | payload len | payload crc32 | rsvd
 *     [16..]    payload: the crash record text
 *
 * CRC checking over a slot reads the payload in small chunks (a slot may be
 * much larger than a record -- e.g. an internal-flash sector), so no big
 * buffer is needed even for large slots.
 */
#include <stddef.h>

#include "tracer_crash_store.h"

#if TRACER_USE_CRASH

/* ---- CRC-32 (IEEE 802.3 poly 0xEDB88320, reflected, bitwise) ------------ */
#define TRACER_CRASH_STORE_MAGIC 0x31434E54u  /* 'TNC1' */
#define TRACER_CRASH_STORE_HDR   16u

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint32_t crc;
    uint32_t rsvd;
} tcs_hdr_t;

/* Single-writer scratch used for chunked CRC reads (kept static, never on a
 * small fault-path stack). */
static uint8_t s_tmp[256u];

static uint32_t tcs_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
    while (n-- != 0u) {
        crc ^= (uint32_t)*p++;
        for (unsigned i = 0u; i < 8u; i++) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return crc;
}

/* ---- weak media defaults: no media configured -> everything fails silently */
#if defined(__ICCARM__)
#pragma weak tracer_crash_store_get_media
#pragma weak tracer_crash_store_erase
#pragma weak tracer_crash_store_write
#pragma weak tracer_crash_store_read
#define TCS_WEAK
#else
#define TCS_WEAK __attribute__((weak))
#endif

int TCS_WEAK tracer_crash_store_get_media(tracer_crash_store_media_t *info) {
    (void)info;
    return -1; /* not configured */
}
int TCS_WEAK tracer_crash_store_erase(uint32_t addr) {
    (void)addr;
    return -1;
}
int TCS_WEAK tracer_crash_store_write(uint32_t addr, const void *buf,
                                      uint32_t len) {
    (void)addr; (void)buf; (void)len;
    return -1;
}
int TCS_WEAK tracer_crash_store_read(uint32_t addr, void *buf, uint32_t len) {
    (void)addr; (void)buf; (void)len;
    return -1;
}

/* ---- slot helpers ------------------------------------------------------- */

/* Read a slot header; returns 0 when it cannot be read. */
static int tcs_hdr_read(uint32_t addr, tcs_hdr_t *h) {
    return tracer_crash_store_read(addr, h, (uint32_t)sizeof(*h)) == 0;
}

/* Verify crc32 over [addr, addr+len) equals @expect, reading in chunks. */
static int tcs_crc_region_ok(uint32_t addr, uint32_t len, uint32_t expect) {
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t off = 0u;
    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > (uint32_t)sizeof(s_tmp)) {
            chunk = (uint32_t)sizeof(s_tmp);
        }
        if (tracer_crash_store_read(addr + off, s_tmp, chunk) != 0) {
            return 0;
        }
        crc = tcs_crc32_update(crc, s_tmp, chunk);
        off += chunk;
    }
    return (~crc) == expect;
}

/* True if slot @slot holds a valid record (magic + length bounds + CRC). */
static int tcs_slot_valid(uint32_t base, uint32_t slot_size, uint32_t slots,
                          uint32_t slot) {
    tcs_hdr_t h;
    uint32_t addr;
    (void)slots;
    if (slot_size < TRACER_CRASH_STORE_HDR + 1u) {
        return 0;
    }
    addr = base + slot * slot_size;
    if (!tcs_hdr_read(addr, &h)) {
        return 0;
    }
    if (h.magic != TRACER_CRASH_STORE_MAGIC || h.len == 0u ||
        h.len > slot_size - TRACER_CRASH_STORE_HDR) {
        return 0;
    }
    return tcs_crc_region_ok(addr + TRACER_CRASH_STORE_HDR, h.len, h.crc);
}

/* Index of the currently valid slot, or -1 if none. */
static int tcs_current(const tracer_crash_store_media_t *m) {
    uint32_t s;
    for (s = 0u; s < m->slot_count; s++) {
        if (tcs_slot_valid(m->slot_base, m->slot_size, m->slot_count, s)) {
            return (int)s;
        }
    }
    return -1;
}

/* ---- store strategy API -------------------------------------------------- */

/* Strong implementation of tracer's weak tracer_crash_save(): persist a
 * finished crash record into the non-current slot.  Silent when no media. */
void tracer_crash_save(const void *record, uint32_t len) {
    tracer_crash_store_media_t m;
    tcs_hdr_t h;
    uint32_t slot;
    uint32_t addr;
    int cur;

    if (record == NULL || len == 0u) {
        return;
    }
    if (tracer_crash_store_get_media(&m) != 0) {
        return; /* no media configured */
    }
    if (m.slot_count == 0u || m.slot_size < TRACER_CRASH_STORE_HDR + 1u ||
        len > m.slot_size - TRACER_CRASH_STORE_HDR) {
        return;
    }

    /* Alternate: write into the slot that is not currently valid. */
    cur = tcs_current(&m);
    if (cur == 0) {
        slot = 1u % m.slot_count;
    } else if (cur > 0) {
        slot = 0u;
    } else {
        slot = 0u;
    }
    /* slot is always < m.slot_count here (m.slot_count >= 1 was checked
     * above, 1 % count < count, and the other branches pick 0). */
    addr = m.slot_base + slot * m.slot_size;

    h.magic = TRACER_CRASH_STORE_MAGIC;
    h.len = len;
    h.crc = ~tcs_crc32_update(0xFFFFFFFFu, (const uint8_t *)record, len);
    h.rsvd = 0u;

    if (tracer_crash_store_erase(addr) != 0) {
        return;
    }
    if (tracer_crash_store_write(addr, &h, (uint32_t)sizeof(h)) != 0) {
        return;
    }
    (void)tracer_crash_store_write(addr + TRACER_CRASH_STORE_HDR, record, len);
}

/* Copy the newest valid record into @buf (up to @cap bytes).  Returns the
 * record length, or 0 when nothing valid is stored. */
uint32_t tracer_crash_store_read_latest(void *buf, uint32_t cap) {
    tracer_crash_store_media_t m;
    int32_t s;

    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    if (tracer_crash_store_get_media(&m) != 0) {
        return 0u;
    }
    for (s = (int32_t)m.slot_count - 1; s >= 0; s--) {
        tcs_hdr_t h;
        uint32_t addr = m.slot_base + (uint32_t)s * m.slot_size;
        if (!tcs_hdr_read(addr, &h)) {
            continue;
        }
        if (h.magic != TRACER_CRASH_STORE_MAGIC || h.len == 0u ||
            h.len > cap || h.len > m.slot_size - TRACER_CRASH_STORE_HDR) {
            continue;
        }
        if (tracer_crash_store_read(addr + TRACER_CRASH_STORE_HDR, buf, h.len) != 0) {
            continue;
        }
        /* Re-verify the CRC over the copied payload. */
        if ((~tcs_crc32_update(0xFFFFFFFFu, (const uint8_t *)buf, h.len)) != h.crc) {
            continue;
        }
        return h.len;
    }
    return 0u;
}

/* Erase every valid slot (normally only one, but a previous half-write or
 * interference could leave more than one valid) so the next boot reports
 * nothing. */
void tracer_crash_store_clear(void) {
    tracer_crash_store_media_t m;
    int cur;
    if (tracer_crash_store_get_media(&m) != 0) {
        return;
    }
    while ((cur = tcs_current(&m)) >= 0) {
        (void)tracer_crash_store_erase(m.slot_base + (uint32_t)cur * m.slot_size);
    }
}

#endif /* TRACER_USE_CRASH */
