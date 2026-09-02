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
 * Build (links tracer_crash_store.c with TRACER_USE_CRASHLOG):
 *   gcc -std=c99 -Wall -Wextra -DTRACER_USE_CRASHLOG=1 \
 *       -I.. tests/test_crash_store.c ../tracer_crash_store.c -o t && ./t
 * (also wired into CTest via -DTRACER_BUILD_TESTS=ON and into CI).
 */
#include <stdio.h>
#include <string.h>

#include "tracer_crash_store.h"

/* ---- fake media: 2 slots x 256 B RAM "flash", erased = 0xFF ---- */
#define FAKE_SLOT   256u
#define FAKE_SLOTS  2u
static unsigned char g_flash[FAKE_SLOT * FAKE_SLOTS];

int tracer_crash_store_get_media(tracer_crash_store_media_t *info) {
    info->base = 0u;
    info->slot_size = FAKE_SLOT;
    info->slots = FAKE_SLOTS;
    return 0;
}
int tracer_crash_store_erase(uint32_t addr) {
    if (addr >= sizeof(g_flash)) {
        return -1;
    }
    memset(g_flash + addr, 0xFF, FAKE_SLOT);
    return 0;
}
int tracer_crash_store_write(uint32_t addr, const void *buf, uint32_t len) {
    if (addr + len > sizeof(g_flash)) {
        return -1;
    }
    memcpy(g_flash + addr, buf, len);
    return 0;
}
int tracer_crash_store_read(uint32_t addr, void *buf, uint32_t len) {
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
    CHECK(g_flash[FAKE_SLOT + 16u] == (unsigned char)rec2[0]); /* rec2 in slot1 */
    g_flash[FAKE_SLOT + 16u + 2u] ^= 0xFFu;  /* flip a payload byte of slot1 */
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

    if (s_fail != 0) {
        fprintf(stderr, "tracer crash_store test: %d FAILURE(S)\n", s_fail);
        return 1;
    }
    printf("tracer crash_store test: all passed\n");
    return 0;
}
