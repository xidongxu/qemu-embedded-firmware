/* Host unit test for the weak block-push async persistence hook
 * (tracer_log_sink) -- the "push" half of the leveled runtime log.
 *
 * tracer.c is compiled as a SEPARATE translation unit here (NOT #included),
 * so its weak tracer_log_sink() default can be replaced by this file's strong
 * definition at link time -- exactly how a real application wires file/flash
 * logging.  A record shorter than TRACER_LOG_SINK_CHUNK_SIZE arrives in ONE
 * sink call (the end-of-call flush); a longer record is split into blocks of
 * exactly TRACER_LOG_SINK_CHUNK_SIZE plus one final remainder block.  The
 * "pull" half (tracer_log_drain) is covered in test_tracer_log.c together
 * with the unified-ring behavior.
 *
 * Build (links tracer.c with TRACER_USE_LOG only; memory macros pinned):
 *   gcc -std=c99 -Wall -Wextra -DTRACER_USE_LOG=1 \
 *       -DTRACER_STACK_DUMP_BYTES=0 \
 *       -DTRACER_STACK_BASE=0x800 -DTRACER_STACK_TOP=0x1000 \
 *       -DTRACER_TEXT_START=0x08000000 -DTRACER_TEXT_END=0x08020000 \
 *       -I.. host-tests/test_tracer_log_sink.c ../tracer.c -o t && ./t
 * (also wired into CTest via -DTRACER_BUILD_TESTS=ON and into CI).
 */
#include <stdio.h>
#include <string.h>

#include "../tracer.h"

static char s_all[8192];
static uint32_t s_all_len = 0u;
static int s_calls = 0;
static uint32_t s_last_len = 0u;
static int s_bad_chunk = 0;   /* any block larger than the chunk size? */

/* Strong override of the weak block-push persistence hook. */
void tracer_log_sink(const void *data, uint32_t len) {
    s_calls++;
    s_last_len = len;
    if (len > TRACER_LOG_SINK_CHUNK_SIZE) {
        s_bad_chunk = 1;
    }
    if (s_all_len + len <= sizeof(s_all)) {
        memcpy(s_all + s_all_len, data, len);
        s_all_len += len;
    } else {
        s_all_len = (uint32_t)-1;   /* overflow marker -> test fails */
    }
}

/* Override: report an unknown stack limit (0), so the on-demand walkers'
 * TRACER_STACK_TOP fallback branch is taken. */
uint32_t tracer_stack_limit(void) {
    return 0u;
}

static void sink_reset(void) {
    s_all_len = 0u;
    s_calls = 0;
    s_last_len = 0u;
    s_bad_chunk = 0;
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
    uint32_t n1, n2, n3, pre;
    static const char big[] =
        "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"
        "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"
        "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"
        "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"
        "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"
        "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"
        "0123456789abcdef" "0123456789abcdef"; /* 20 x 16 = 320 B */

    tracer_log_set_level(TRACER_LOG_TRACE);

    /* 1. a short record (< chunk) arrives as ONE sink call, exact bytes. */
    sink_reset();
    n1 = tracer_log(TRACER_LOG_INFO, "sink %d", 7);
    CHECK(n1 == s_all_len);
    CHECK(s_calls == 1);               /* end-of-call flush */
    CHECK(s_last_len == n1);
    CHECK(memcmp(s_all, "[0][I] sink 7\r\n", s_all_len) == 0);
    CHECK(s_bad_chunk == 0);

    /* 2. filtered records must NOT reach the sink. */
    tracer_log_set_level(TRACER_LOG_ERROR);
    CHECK(tracer_log(TRACER_LOG_INFO, "nope") == 0u);
    CHECK(s_calls == 1);
    CHECK(tracer_log_get_level() == TRACER_LOG_ERROR);

    /* 3. lowering the runtime level re-enables the sink push. */
    tracer_log_set_level(TRACER_LOG_DEBUG);
    n2 = tracer_log(TRACER_LOG_DEBUG, "dbg %x", 0x1Au);
    CHECK(n2 > 0u);
    CHECK(s_calls == 2);
    CHECK(memcmp(s_all,
                 "[0][I] sink 7\r\n[0][D] dbg 1a\r\n",
                 s_all_len) == 0);
    CHECK(s_bad_chunk == 0);

    /* 4. a long record is split into full blocks + a final remainder; the
     * re-assembled bytes equal the whole streamed record, verbatim. */
    sink_reset();
    n3 = tracer_log(TRACER_LOG_INFO, "%s", big);
    pre = (uint32_t)strlen("[0][I] ");
    CHECK(s_all_len == n3);                          /* nothing lost */
    CHECK(memcmp(s_all + pre, big, strlen(big)) == 0); /* full 320 B body */
    CHECK(s_all[n3 - 1u] == '\n');
    CHECK(s_bad_chunk == 0);                         /* no oversized block */
    CHECK(s_calls ==
          (int)((n3 + TRACER_LOG_SINK_CHUNK_SIZE - 1u) /
                TRACER_LOG_SINK_CHUNK_SIZE));
    CHECK(s_last_len == n3 % TRACER_LOG_SINK_CHUNK_SIZE); /* remainder last */

    /* 5. unified ring: two short records drain as one stream, in order.
     * (First consume whatever sections 1-4 streamed into the shared ring, so
     * the drain below only sees the two new records.) */
    sink_reset();
    {
        static uint8_t junk[1024];
        (void)tracer_log_drain(junk, sizeof(junk));
    }
    n1 = tracer_log(TRACER_LOG_INFO, "aaa");
    n2 = tracer_log(TRACER_LOG_WARN, "bbb");
    {
        static uint8_t out[512];
        uint32_t d = tracer_log_drain(out, sizeof(out));
        CHECK(d == n1 + n2);
        CHECK(d == s_all_len);
        CHECK(memcmp(out, "[0][I] aaa\r\n[0][W] bbb\r\n", d) == 0);
        CHECK(tracer_log_drain(out, sizeof(out)) == 0u);
    }

    /* 6. on-demand call-stack snapshot through the REAL renderer (the empty
     * TRACER_PRINTF builds cannot reach these lines): exercises the walker
     * fallback (stack_limit() == 0) and the banner output. */
    sink_reset();
    {
        uint32_t buf[8];
        CHECK(tracer_get_callstack(buf, 8u) == 0u);
    }
    tracer_dump_callstack();

    if (s_fail != 0) {
        fprintf(stderr, "tracer log sink test: %d FAILURE(S)\n", s_fail);
        return 1;
    }
    printf("tracer log sink test: all passed\n");
    return 0;
}
