/* Host test for tracer_crash_store's WEAK media default stubs.
 *
 * The main test (test_crash_store.c) links a full RAM-flash driver, which
 * shadows the library's weak media primitives.  Here we deliberately link
 * only PART of the media layer so the remaining weak defaults execute and
 * are locked in as "fail silently, never crash":
 *
 *   TCS_STUB_MODE=0  no driver at all           -> weak get_media  (-1)
 *   TCS_STUB_MODE=1  get_media only             -> weak erase/write/read
 *   TCS_STUB_MODE=2  get_media + erase          -> weak write/read
 *
 * Build (per mode):
 *   gcc -std=c99 -Wall -Wextra -DTRACER_USE_CRASH=1 -DTCS_STUB_MODE=1 \
 *       -I. tests/host/test_crash_store_stubs.c tracer_crash_store.c -o t && ./t
 * (also wired into CTest / the coverage run).
 */
#include <stdio.h>

#include "tracer_crash_store.h"

#ifndef TCS_STUB_MODE
#define TCS_STUB_MODE 0
#endif

/* Provide a working get_media (and, in mode 2, a working erase) so that
 * save/clear/read_latest get far enough to hit the still-weak write/read. */
#if TCS_STUB_MODE != 0
int tracer_crash_store_get_media(tracer_crash_store_media_t *info) {
    info->slot_base = 0u;
    info->slot_size = 256u;
    info->slot_count = 2u;
    return 0;
}
#endif
#if TCS_STUB_MODE == 2
int tracer_crash_store_erase(uint32_t addr) {
    (void)addr;
    return 0;
}
#endif

static int s_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_fail++;                                                        \
        }                                                                    \
    } while (0)

int main(void) {
    static const char rec[] = "CRASH-REC weak-media\r\n";
    static char out[256];

    /* Every store API must be a SILENT no-op when the media layer is missing
     * or half-wired: no crash, no fabricated record. */
    tracer_crash_save(rec, (uint32_t)(sizeof(rec) - 1u));
    CHECK(tracer_crash_store_read_latest(out, sizeof(out)) == 0u);
    tracer_crash_save(rec, 0u);
    tracer_crash_save(NULL, 8u);
    tracer_crash_store_clear();
    CHECK(tracer_crash_store_read_latest(NULL, 0u) == 0u);
    tracer_crash_store_read_latest(out, sizeof(out)); /* double-read is fine */

    if (s_fail != 0) {
        fprintf(stderr, "crash_store stub test (mode %d): %d FAILURE(S)\n",
                (int)TCS_STUB_MODE, s_fail);
        return 1;
    }
    printf("crash_store stub test (mode %d): all passed\n", (int)TCS_STUB_MODE);
    return 0;
}
