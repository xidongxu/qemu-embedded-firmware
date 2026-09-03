/* Host unit test for tracer crash-log ("black box"): pre-crash ring log,
 * dump capture mirror and CRC footer.
 *
 * Compiles tracer.c on the HOST with TRACER_USE_CRASH=1 and drives
 * tracer_ring_printf() plus a simulated fault dump (mini-printf while the
 * capture is active) against a collecting sink, then verifies the assembled
 * record in the capture buffer.
 *
 * Run:  gcc -std=c99 -Wall -Wextra -I.. tests/test_crashlog.c -o t && ./t
 * (also wired into CTest via -DTRACER_BUILD_TESTS=ON).
 *
 * The ring / capture buffers and helpers are static in tracer.c; including
 * the file puts us in the same translation unit so we can inspect them.
 */
#include <stdio.h>
#include <string.h>

static char s_ser[4096];
static size_t s_ser_len = 0u;
static void tr_putc(char c) {
    if (s_ser_len + 1u < sizeof(s_ser)) {
        s_ser[s_ser_len++] = c;
    }
}

/* Small sizes so the tests also exercise ring overwrite + capture bounds. */
#define TRACER_RING_SIZE 128u
#define TRACER_CRASH_SIZE 512u
#define TRACER_USE_CRASH 1
#define TRACER_PUTCHAR tr_putc
#define TRACER_STACK_DUMP_BYTES 0u
#define TRACER_STACK_BASE 0x20000000u
#define TRACER_STACK_TOP  0x20010000u
#define TRACER_TEXT_START 0x08000000u
#define TRACER_TEXT_END   0x08020000u
#include "../tracer.c"

static int s_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_fail++;                                                        \
        }                                                                    \
    } while (0)

/* Sub-string search over a non-NUL-terminated byte range. */
static int contains(const char *hay, uint32_t hlen, const char *needle) {
    uint32_t nlen = (uint32_t)strlen(needle);
    uint32_t i;
    if (nlen > hlen) {
        return 0;
    }
    for (i = 0u; i + nlen <= hlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            return 1;
        }
    }
    return 0;
}

int main(void) {
    uint32_t i;

    /* ---- 1. ring keeps the most recent bytes, oldest dropped ---- */
    for (i = 0u; i < 100u; i++) {
        tracer_ring_printf("evt%u|", (unsigned)i);
    }
    /* ~600 B written into a 128 B ring: the oldest bytes are gone. */
    {
        uint32_t n = s_ring_count;
        uint32_t idx = s_ring_start;
        char first[4];
        for (i = 0u; i < 4u; i++) {
            first[i] = (char)s_ring[(idx + i) % sizeof(s_ring)];
        }
        CHECK(n == sizeof(s_ring));                       /* full ring */
        CHECK(memcmp(first, "evt", 3u) != 0);             /* evt0.. gone */
    }

    /* ---- 2. capture: mirror a "dump", then ring tail + CRC footer ---- */
    s_cap_len = 0u;
    s_cap_active = 0;
    tracer_cap_begin();
    /* Simulate a fault dump going out through the mini-printf. */
    tracer_xprintf("\r\n===== Tracer: Test Fault Dump =====\r\n");
    tracer_xprintf("FW     : 9.9.9\r\n");
    tracer_xprintf("PC =%08lX  LR =%08lX  SP =%08lX\r\n",
                   (unsigned long)0x1000u, (unsigned long)0x1004u,
                   (unsigned long)0x2000FFFCu);
    tracer_xprintf("Call stack:");
    tracer_xprintf(" 10001111");
    tracer_xprintf("\r\n");
    /* The capture must also have mirrored what went to the serial sink. */
    tracer_crash_finalize();

    CHECK(s_cap_active == 0);
    CHECK(contains((const char *)s_cap, (uint32_t)s_cap_len,
                   "Test Fault Dump"));
    CHECK(contains((const char *)s_cap, (uint32_t)s_cap_len,
                   "PC =00001000"));
    CHECK(contains((const char *)s_cap, (uint32_t)s_cap_len,
                   "Call stack: 10001111"));
    CHECK(contains((const char *)s_cap, (uint32_t)s_cap_len,
                   "==== Recent ring log (pre-crash) ===="));
    CHECK(contains((const char *)s_cap, (uint32_t)s_cap_len, "evt99|"));
    CHECK(contains((const char *)s_cap, (uint32_t)s_cap_len,
                   "==== TRACER CRASHLOG END crc="));

    /* ---- 3. ring writes must NOT leak into the serial sink ---- */
    s_ser_len = 0u;
    tracer_ring_printf("ringonly\n");
    CHECK(s_ser_len == 0u);

    if (s_fail != 0) {
        fprintf(stderr, "tracer crashlog test: %d FAILURE(S)\n", s_fail);
        return 1;
    }
    printf("tracer crashlog test: all passed\n");
    return 0;
}
