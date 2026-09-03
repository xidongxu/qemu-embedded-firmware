/* Host unit test for the weak per-line async persistence hook
 * (tracer_log_sink) -- the "push" half of the leveled runtime log.
 *
 * tracer.c is compiled as a SEPARATE translation unit here (NOT #included),
 * so its weak tracer_log_sink() default can be replaced by this file's strong
 * definition at link time -- exactly how a real application wires file/flash
 * logging.  The "pull" half (tracer_log_drain) is covered in test_tracer_log.c
 * together with the unified-ring behavior.
 *
 * Build (links tracer.c with TRACER_USE_LOG only; memory macros pinned):
 *   gcc -std=c99 -Wall -Wextra -DTRACER_USE_LOG=1 \
 *       -DTRACER_STACK_DUMP_BYTES=0 \
 *       -DTRACER_STACK_BASE=0x20000000 -DTRACER_STACK_TOP=0x20010000 \
 *       -DTRACER_TEXT_START=0x08000000 -DTRACER_TEXT_END=0x08020000 \
 *       -I.. tests/test_tracer_log_sink.c ../tracer.c -o t && ./t
 * (also wired into CTest via -DTRACER_BUILD_TESTS=ON and into CI).
 */
#include <stdio.h>
#include <string.h>

#include "../tracer.h"

static int s_calls = 0;
static char s_line[160];
static uint32_t s_line_len = 0u;

/* Strong override of the weak per-line persistence hook. */
void tracer_log_sink(const void *line, uint32_t len) {
    s_calls++;
    s_line_len = (len <= sizeof(s_line)) ? len : (uint32_t)sizeof(s_line);
    memcpy(s_line, line, s_line_len);
}

static int s_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_fail++;                                                        \
        }                                                                    \
    } while (0)

int main(void) {
    uint32_t n_info, n_dbg;

    /* 1. every passing line is pushed whole to the sink, exact bytes. */
    tracer_log_set_level(TRACER_LOG_TRACE);
    n_info = tracer_log(TRACER_LOG_INFO, "sink %d", 7);
    CHECK(s_calls == 1);
    CHECK(n_info == s_line_len);
    CHECK(s_line_len == (uint32_t)strlen("[0 ms] I: sink 7\r\n"));
    CHECK(memcmp(s_line, "[0 ms] I: sink 7\r\n", s_line_len) == 0);

    /* 2. filtered lines must NOT reach the sink. */
    tracer_log_set_level(TRACER_LOG_ERROR);
    CHECK(tracer_log(TRACER_LOG_INFO, "nope") == 0u);
    CHECK(s_calls == 1);
    CHECK(tracer_log_get_level() == TRACER_LOG_ERROR);

    /* 3. lowering the runtime level re-enables the sink push. */
    tracer_log_set_level(TRACER_LOG_DEBUG);
    n_dbg = tracer_log(TRACER_LOG_DEBUG, "dbg %x", 0x1Au);
    CHECK(s_calls == 2);
    CHECK(n_dbg == s_line_len);
    CHECK(s_line_len == (uint32_t)strlen("[0 ms] D: dbg 1a\r\n"));
    CHECK(memcmp(s_line, "[0 ms] D: dbg 1a\r\n", s_line_len) == 0);

    /* 4. unified ring: the two passing lines drain as one continuous
     * stream, in order; the next drain has nothing new. */
    {
        static uint8_t out[512];
        uint32_t d;
        d = tracer_log_drain(out, sizeof(out));
        CHECK(d == n_info + n_dbg);
        CHECK(memcmp(out, "[0 ms] I: sink 7\r\n[0 ms] D: dbg 1a\r\n", d) == 0);
        CHECK(tracer_log_drain(out, sizeof(out)) == 0u);
    }

    if (s_fail != 0) {
        fprintf(stderr, "tracer log sink test: %d FAILURE(S)\n", s_fail);
        return 1;
    }
    printf("tracer log sink test: all passed\n");
    return 0;
}
