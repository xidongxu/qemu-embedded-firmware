/* Host unit test for tracer_crash_store.c -- the media-agnostic crash
 * record store (double-slot strategy).  Provides a RAM "flash" implementing
 * the weak media primitives, then verifies:
 *   - no record -> read_latest() == 0
 *   - save -> read_latest() returns the identical record
 *   - second save alternates to the other slot
 *   - a corrupted (half-written) slot fails its CRC and is skipped, the
 *     other slot's older record is still returned
 *   - clear() removes the record (read_latest() == 0)
 *
 * Build (links tracer_crash_store.c with TRACER_USE_CRASH):
 *   gcc -std=c99 -Wall -Wextra -DTRACER_USE_CRASH=1 \
 *       -I.. host-tests/test_crash_store.c ../tracer_crash_store.c -o t && ./t
 * (also wired into CTest via -DTRACER_BUILD_TESTS=ON and into CI).
 */
#include <stdio.h>
#include <string.h>

#include "tracer_crash_store.h"

/* ---- fake media: erased = 0xFF RAM "flash".  The slot geometry is
 * runtime-configurable (g_slot_size / g_slot_count) so the same media can
 * emulate normal 256-B slots, single-slot and tiny-slot media, and large
 * 1-KiB slots (which exercise the chunked CRC reads). */
#define FAKE_SLOT_DEFAULT 256u
#define FAKE_FLASH_BYTES  2048u   /* big enough for the 2 x 1 KiB geometry */
static unsigned char g_flash[FAKE_FLASH_BYTES];
static uint32_t g_slot_size = FAKE_SLOT_DEFAULT;
static uint32_t g_slot_count = 2u;

/* Failure-injection knobs to reach the error/early-return paths. */
static int g_no_media = 0;
static int g_fail_erase = 0;
static int g_fail_write = 0;
static int g_fail_read = 0;       /* every read fails */
static int g_fail_read_data = 0;  /* only payload reads (len > 16) fail */

int tracer_crash_store_get_media(tracer_crash_store_media_t *info) {
    if (g_no_media) {
        return -1;
    }
    info->slot_base = 0u;
    info->slot_size = g_slot_size;
    info->slot_count = g_slot_count;
    return 0;
}
int tracer_crash_store_erase(uint32_t addr) {
    if (g_fail_erase) {
        return -1;
    }
    if (addr >= sizeof(g_flash)) {
        return -1;
    }
    memset(g_flash + addr, 0xFF, g_slot_size);
    return 0;
}
int tracer_crash_store_write(uint32_t addr, const void *buf, uint32_t len) {
    if (g_fail_write) {
        return -1;
    }
    if (addr + len > sizeof(g_flash)) {
        return -1;
    }
    memcpy(g_flash + addr, buf, len);
    return 0;
}
int tracer_crash_store_read(uint32_t addr, void *buf, uint32_t len) {
    /* The 16-B header reads have len == 16; payload reads are longer. */
    if (g_fail_read || (g_fail_read_data && len > 16u)) {
        return -1;
    }
    if (addr + len > sizeof(g_flash)) {
        return -1;
    }
    memcpy(buf, g_flash + addr, len);
    return 0;
}

static int s_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_fail++;                                                        \
        }                                                                    \
    } while (0)

static void flash_erase_all(void) {
    memset(g_flash, 0xFF, sizeof(g_flash));
}

int main(void) {
    static const char rec1[] = "CRASH-REC-1 r0 r1 cfsr\r\n";
    static const char rec2[] = "CRASH-REC-2 pc=1000 sp=2000\r\n";
    static char out[512];
    uint32_t n;

    flash_erase_all();

    /* 1. nothing stored yet. */
    CHECK(tracer_crash_store_read_latest(out, sizeof(out)) == 0u);

    /* 2. save -> slot 0, read back identical. */
    tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u));
    n = tracer_crash_store_read_latest(out, sizeof(out));
    CHECK(n == (uint32_t)(sizeof(rec1) - 1u));
    CHECK(memcmp(out, rec1, n) == 0);

    /* 3. second save alternates to slot 1; newest is rec2. */
    tracer_crash_save(rec2, (uint32_t)(sizeof(rec2) - 1u));
    n = tracer_crash_store_read_latest(out, sizeof(out));
    CHECK(n == (uint32_t)(sizeof(rec2) - 1u));
    CHECK(memcmp(out, rec2, n) == 0);

    /* 4. corrupt slot 1's payload (slot 1 holds rec2; simulate a half-
     * written crash): it now fails its CRC, so read_latest must fall back to
     * slot 0's older rec1. */
    CHECK(g_flash[g_slot_size + 16u] == (unsigned char)rec2[0]); /* rec2 in slot1 */
    g_flash[g_slot_size + 16u + 2u] ^= 0xFFu;  /* flip a payload byte of slot1 */
    n = tracer_crash_store_read_latest(out, sizeof(out));
    CHECK(n == (uint32_t)(sizeof(rec1) - 1u));
    CHECK(memcmp(out, rec1, n) == 0);

    /* 5. a new save still works after the corruption (it picks the invalid
     * slot) and overwrites the corrupt data. */
    tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u));
    n = tracer_crash_store_read_latest(out, sizeof(out));
    CHECK(n == (uint32_t)(sizeof(rec1) - 1u));
    CHECK(memcmp(out, rec1, n) == 0);

    /* 6. clear removes the record. */
    tracer_crash_store_clear();
    CHECK(tracer_crash_store_read_latest(out, sizeof(out)) == 0u);

    /* 7. bad arguments are silent no-ops. */
    tracer_crash_save(NULL, 5u);
    tracer_crash_save("x", 0u);
    CHECK(tracer_crash_store_read_latest(NULL, 100u) == 0u);
    CHECK(tracer_crash_store_read_latest(out, 0u) == 0u);

    /* 8. no media configured: the whole store is a silent no-op. */
    flash_erase_all();
    g_no_media = 1;
    tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u));
    CHECK(tracer_crash_store_read_latest(out, sizeof(out)) == 0u);
    tracer_crash_store_clear();
    g_no_media = 0;

    /* 9. a record larger than a slot is rejected (nothing written). */
    flash_erase_all();
    {
        static char big[250];          /* > 256 - 16 header */
        memset(big, 'A', sizeof(big));
        tracer_crash_save(big, sizeof(big));
    }
    CHECK(tracer_crash_store_read_latest(out, sizeof(out)) == 0u);

    /* 10. erase failure during save leaves the previous record intact. */
    flash_erase_all();
    tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u));
    g_fail_erase = 1;
    tracer_crash_save(rec2, (uint32_t)(sizeof(rec2) - 1u));
    g_fail_erase = 0;
    n = tracer_crash_store_read_latest(out, sizeof(out));
    CHECK(n == (uint32_t)(sizeof(rec1) - 1u));
    CHECK(memcmp(out, rec1, n) == 0);

    /* 11. read failure while scanning invalidates slots -> none found. */
    flash_erase_all();
    tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u));
    g_fail_read = 1;
    CHECK(tracer_crash_store_read_latest(out, sizeof(out)) == 0u);
    g_fail_read = 0;

    /* 12. header-write failure during save is silent; old record stays. */
    flash_erase_all();
    tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u));
    g_fail_write = 1;
    tracer_crash_save(rec2, (uint32_t)(sizeof(rec2) - 1u));
    g_fail_write = 0;
    n = tracer_crash_store_read_latest(out, sizeof(out));
    CHECK(n == (uint32_t)(sizeof(rec1) - 1u));
    CHECK(memcmp(out, rec1, n) == 0);

    /* 13. save while EVERY read fails: tcs_current cannot validate a slot
     * (header read fails) and must still not crash (slot 0 rewritten). */
    flash_erase_all();
    tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u));
    g_fail_read = 1;
    tracer_crash_save(rec2, (uint32_t)(sizeof(rec2) - 1u));
    g_fail_read = 0;
    n = tracer_crash_store_read_latest(out, sizeof(out));
    CHECK(n == (uint32_t)(sizeof(rec2) - 1u));

    /* 14. only PAYLOAD reads fail: the CRC-region read in tcs_slot_valid
     * (save path) and the direct payload read in read_latest drop the slot. */
    flash_erase_all();
    tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u));
    g_fail_read_data = 1;
    CHECK(tracer_crash_store_read_latest(out, sizeof(out)) == 0u); /* 207 */
    tracer_crash_save(rec2, (uint32_t)(sizeof(rec2) - 1u));         /* 95 */
    g_fail_read_data = 0;
    n = tracer_crash_store_read_latest(out, sizeof(out));
    CHECK(n == (uint32_t)(sizeof(rec2) - 1u));

    /* 15. slot-0 invalid + slot-1 valid: save alternates back to slot 0
     * (the "current slot is 1" branch). */
    flash_erase_all();
    tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u)); /* slot0 = rec1 */
    tracer_crash_save(rec2, (uint32_t)(sizeof(rec2) - 1u)); /* slot1 = rec2 */
    g_flash[0] ^= 0xFFu;              /* corrupt slot0 magic -> slot0 invalid */
    tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u)); /* cur==1 -> slot0 */
    n = tracer_crash_store_read_latest(out, sizeof(out));
    CHECK(n == (uint32_t)(sizeof(rec2) - 1u));              /* newest = slot1 */
    CHECK(memcmp(g_flash + 16u, rec1, sizeof(rec1) - 1u) == 0); /* rec1 in slot0 */

    /* 16. single-slot media with slot 0 valid: save stays in slot 0. */
    flash_erase_all();
    g_slot_count = 1u;
    tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u));
    tracer_crash_save(rec2, (uint32_t)(sizeof(rec2) - 1u)); /* cur==0 -> 1%1==0 */
    n = tracer_crash_store_read_latest(out, sizeof(out));
    CHECK(n == (uint32_t)(sizeof(rec2) - 1u));
    g_slot_count = 2u;

    /* 17. a slot too small for a header: save rejects it up front and
     * clear() walks the slot-valid size guard without crashing. */
    flash_erase_all();
    g_slot_size = 8u;
    tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u)); /* rejected */
    tracer_crash_store_clear();                              /* size guard */
    CHECK(tracer_crash_store_read_latest(out, sizeof(out)) == 0u);
    g_slot_size = FAKE_SLOT_DEFAULT;

    /* 18. large 1-KiB slot: CRC verification of a >256-B record reads the
     * payload in chunks (scratch buffer capped) -> chunked-crc path. */
    flash_erase_all();
    g_slot_size = 1024u;
    {
        static char big[400];
        memset(big, 'B', sizeof(big));
        tracer_crash_save(big, sizeof(big));   /* slot0 = big (len 400 > 256) */
        tracer_crash_save(rec1, (uint32_t)(sizeof(rec1) - 1u)); /* chunked CRC */
        n = tracer_crash_store_read_latest(out, sizeof(out));
        CHECK(n == (uint32_t)(sizeof(rec1) - 1u)); /* newest = slot1 rec1 */
    }
    g_slot_size = FAKE_SLOT_DEFAULT;

    if (s_fail != 0) {
        fprintf(stderr, "tracer crash_store test: %d FAILURE(S)\n", s_fail);
        return 1;
    }
    printf("tracer crash_store test: all passed\n");
    return 0;
}
