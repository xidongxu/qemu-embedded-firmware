/* Host unit test for the tracer leveled runtime log (TRACER_USE_LOG +
 * TRACER_USE_CRASH, so the crash-record test can check that a crash record
 * auto-contains recent logs):
 *   - default runtime level = INFO, filtered lines produce nothing;
 *   - tracer_log() writes the SAME line to the serial sink and the shared
 *     pre-crash ring ("unified" -- so a crash record auto-contains recent
 *     run logs without any extra persist call);
 *   - the "[<ms>][X]" prefix (bare tick + level letter in brackets);
 *   - runtime level can be raised / lowered with tracer_log_set_level();
 *   - tracer_log_drain(): incremental pull, then nothing new, then more, and
 *     the overwrite case (consumer slower than the ring);
 *   - long records stream in full (printf-like, no line-length limit);
 *   - a fault dump capture ends with the recent log (black-box replay).
 *
 * The weak per-record sink is NOT overridden here (default no-op path); the
 * override is tested separately in test_tracer_log_sink.c because the weak
 * definition lives in this same translation unit (tracer.c is #included).
 *
 * Run:  gcc -std=c99 -Wall -Wextra -I.. tests/test_tracer_log.c -o t && ./t
 * (also wired into CTest via -DTRACER_BUILD_TESTS=ON).
 */
#include <stdio.h>
#include <string.h>

static char s_ser[8192];
static size_t s_ser_len = 0u;
static void tr_putc(char c) {
    if (s_ser_len + 1u < sizeof(s_ser)) {
        s_ser[s_ser_len++] = c;
    }
}

/* Small sizes so the tests exercise ring overwrite. */
#define TRACER_RING_SIZE 256u
#define TRACER_CRASH_SIZE 1024u
#define TRACER_USE_CRASH 1
#define TRACER_USE_LOG 1
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

static int contains(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    size_t i;
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

static void reset_all(void) {
    s_ser_len = 0u;
    s_ring_start = 0u;
    s_ring_count = 0u;
    s_ring_total = 0u;
    s_log_drain_at = 0u;
    s_cap_len = 0u;
    s_cap_active = 0;
    tracer_log_set_level(TRACER_LOG_INFO);
}

int main(void) {
    uint32_t n, i;

    /* ---- 1. default runtime level INFO: DEBUG filtered out, INFO kept ---- */
    reset_all();
    CHECK(tracer_log_get_level() == TRACER_LOG_INFO);
    n = tracer_log(TRACER_LOG_DEBUG, "dropped %d", 1);
    CHECK(n == 0u);                    /* filtered */
    CHECK(s_ser_len == 0u);            /* nothing on the serial sink */
    CHECK(s_ring_count == 0u);         /* nothing in the shared ring */
    n = TRACER_LOGI("hello %d", 42);   /* no-level convenience macro */
    CHECK(n > 0u);
    CHECK(contains(s_ser, s_ser_len, "[0][I] hello 42\r\n"));
    CHECK(s_ring_count == n);          /* unified: ring got the same line */

    /* ---- 2. level letters + runtime switch up and down ---- */
    reset_all();
    tracer_log_set_level(TRACER_LOG_TRACE);
    CHECK(tracer_log_get_level() == TRACER_LOG_TRACE);
    CHECK(tracer_log(TRACER_LOG_TRACE, "t") > 0u);
    CHECK(tracer_log(TRACER_LOG_DEBUG, "d") > 0u);
    CHECK(tracer_log(TRACER_LOG_INFO, "i") > 0u);
    CHECK(tracer_log(TRACER_LOG_WARN, "w") > 0u);
    CHECK(tracer_log(TRACER_LOG_ERROR, "e") > 0u);
    CHECK(contains(s_ser, s_ser_len, "[0][T] t\r\n"));
    CHECK(contains(s_ser, s_ser_len, "[0][D] d\r\n"));
    CHECK(contains(s_ser, s_ser_len, "[0][W] w\r\n"));
    CHECK(contains(s_ser, s_ser_len, "[0][E] e\r\n"));
    tracer_log_set_level(TRACER_LOG_ERROR);   /* runtime raise: fewer logs */
    CHECK(tracer_log(TRACER_LOG_WARN, "w2") == 0u);
    CHECK(tracer_log(TRACER_LOG_ERROR, "e2") > 0u);
    CHECK(contains(s_ser, s_ser_len, "[0][E] e2\r\n"));

    /* ---- 3. drain: incremental pull then nothing, then more ---- */
    reset_all();
    tracer_log_set_level(TRACER_LOG_TRACE);
    n = tracer_log(TRACER_LOG_INFO, "aaa");
    {
        static char out[512];
        uint32_t d = tracer_log_drain((uint8_t *)out, sizeof(out));
        CHECK(d == n);                 /* first drain returns the whole line */
        CHECK(memcmp(out, "[0][I] aaa\r\n", d) == 0);
        CHECK(tracer_log_drain((uint8_t *)out, sizeof(out)) == 0u); /* done */
    }
    n = tracer_log(TRACER_LOG_WARN, "bbb");
    {
        static char out[512];
        uint32_t d = tracer_log_drain((uint8_t *)out, sizeof(out));
        CHECK(d == n);                 /* only the new line this time */
        CHECK(memcmp(out, "[0][W] bbb\r\n", d) == 0);
    }

    /* ---- 4. drain when the consumer is too slow: ring already ---- */
    /*      overwrote part of the stream -> oldest available is returned. */
    reset_all();
    tracer_log_set_level(TRACER_LOG_TRACE);
    for (i = 0u; i < 20u; i++) {       /* ~20 x 40 B >> 256 B ring */
        tracer_log(TRACER_LOG_INFO, "flood line %u marker", (unsigned)i);
    }
    CHECK(s_ring_count == TRACER_RING_SIZE);            /* full */
    s_log_drain_at = 0u;               /* pretend we never drained */
    {
        static char out[512];
        static char rb[512];
        uint32_t d = tracer_log_drain((uint8_t *)out, sizeof(out));
        uint32_t m = s_ring_count;
        uint32_t idx = s_ring_start;
        CHECK(d == TRACER_RING_SIZE);           /* exactly what is left */
        CHECK(s_log_drain_at == s_ring_total);  /* cursor at the stream end */
        for (i = 0u; i < m; i++) {      /* compare with ring oldest-first */
            rb[i] = (char)s_ring[idx];
            idx = (idx + 1u) % TRACER_RING_SIZE;
        }
        CHECK(memcmp(out, rb, m) == 0);
        CHECK(tracer_log_drain((uint8_t *)out, sizeof(out)) == 0u);
    }

    /* ---- 5. long records stream in full (no length limit) ---- */
    reset_all();
    tracer_log_set_level(TRACER_LOG_TRACE);
    {
        /* 20 x 16 = 320 B of body, far past any line-buffer limit. */
        static const char big[] =
            "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"
            "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"
            "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"
            "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"
            "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"
            "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"
            "0123456789abcdef" "0123456789abcdef";
        uint32_t pre = (uint32_t)strlen("[0][I] ");
        uint32_t n2;
        n2 = tracer_log(TRACER_LOG_INFO, "%s", big);
        CHECK(n2 == pre + (uint32_t)strlen(big) + 2u); /* not truncated */
        CHECK(s_ser_len == n2);
        CHECK(s_ser[n2 - 2u] == '\r');  /* record still CRLF-terminated */
        CHECK(s_ser[n2 - 1u] == '\n');
        /* the whole 320 B body appears verbatim on the serial sink. */
        CHECK(contains(s_ser, s_ser_len, big));
        /* the (bounded) ring got every byte too -- oldest wrapped off. */
        CHECK(s_ring_total == n2);
        CHECK(s_ring_count == TRACER_RING_SIZE);
    }

    /* ---- 6. crash capture auto-includes the recent run log ---- */
    reset_all();
    tracer_log_set_level(TRACER_LOG_TRACE);
    tracer_log(TRACER_LOG_ERROR, "pre-crash marker %d", 1234);
    s_cap_len = 0u;
    s_cap_active = 0;
    tracer_cap_begin();
    tracer_xprintf("\r\nDUMP-TEXT\r\n");
    tracer_crash_finalize();
    CHECK(contains((const char *)s_cap, (uint32_t)s_cap_len, "DUMP-TEXT"));
    CHECK(contains((const char *)s_cap, (uint32_t)s_cap_len,
                   "==== Recent ring log (pre-crash) ===="));
    CHECK(contains((const char *)s_cap, (uint32_t)s_cap_len,
                   "pre-crash marker 1234"));
    CHECK(contains((const char *)s_cap, (uint32_t)s_cap_len,
                   "==== TRACER CRASHLOG END crc="));

    if (s_fail != 0) {
        fprintf(stderr, "tracer log test: %d FAILURE(S)\n", s_fail);
        return 1;
    }
    printf("tracer log test: all passed\n");
    return 0;
}
