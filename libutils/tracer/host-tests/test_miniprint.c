/* Host unit test for tracer's lock-free mini-printf (TRACER_PUTCHAR mode).
 *
 * Compiles tracer.c on the HOST (gcc/clang, no ARM toolchain) and drives its
 * internal tracer_xprintf against a collecting sink, verifying the exact
 * format subset the fault dump emits (%s %c %d %u %x %X %p, 'l'/'ll'
 * lengths, .N / %.*s precision, %% escape, with '-'/'0' flags and decimal
 * width).
 *
 * Run:  gcc -std=c99 -Wall -Wextra -I.. host-tests/test_miniprint.c -o t && ./t
 * (also wired into CTest via -DTRACER_BUILD_TESTS=ON).
 *
 * Note: the host may be LP64 (Unix `long` = 64-bit) or LLP64 (Windows
 * `long` = 32-bit); the tests below only use small values whose low 32 bits
 * are what the 32-bit ARM target prints.  %p is the exception: it is widened
 * to unsigned long long inside the mini-printf, so a 64-bit host pointer
 * prints in full either way.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char s_buf[512];
static size_t s_len = 0;
static void tr_putc(char c) {
    if (s_len + 1u < sizeof(s_buf)) {
        s_buf[s_len++] = c;
    }
}

#define TRACER_PUTCHAR tr_putc
#define TRACER_STACK_DUMP_BYTES 0u
/* Host has no _sstack/_estack/_stext/_etext (ARM linker-script symbols);
 * the raw dump and code-region checks are disabled here, so harmless
 * constants are enough to satisfy the linker. */
/* Host test: fake stack region must be below any real host stack (the
 * dump-style walkers scan up to TRACER_STACK_TOP; a high top would cross
 * unmapped host pages and crash under ASLR). */
#define TRACER_STACK_BASE 0x00000800u
#define TRACER_STACK_TOP  0x00001000u
#define TRACER_TEXT_START 0x08000000u
#define TRACER_TEXT_END   0x08020000u
#include "../tracer.c"

static int s_failures = 0;

static void reset(void) {
    s_len = 0u;
    s_buf[0] = '\0';
}

static void check(const char *expected, const char *what) {
    s_buf[s_len] = '\0';
    if (strcmp(s_buf, expected) != 0) {
        fprintf(stderr, "FAIL %s: got '%s' want '%s'\n", what, s_buf, expected);
        s_failures++;
    }
}

int main(void) {
    /* Exercise tracer_init()/tracer_dump_header() through the REAL renderer
     * so the fw/text/stack banner lines are covered, then throw it away. */
    reset();
    tracer_init();
    tracer_dump_header("Unit");
    reset();

    /* Register / frame lines as emitted by the fault dump. */
    reset();
    tracer_xprintf(" R12=%08lX  SP =%08lX  LR =%08lX  PC =%08lX\r\n",
                   (unsigned long)0x35u, (unsigned long)0x380FFF70u,
                   (unsigned long)0x100008D3u, (unsigned long)0x10002152u);
    check(" R12=00000035  SP =380FFF70  LR =100008D3  PC =10002152\r\n", "regs");

    reset();
    tracer_xprintf(" CFSR=%08lX  MMFSR=%02lX  BFSR=%02lX  UFSR=%04lX\r\n",
                   (unsigned long)0x01000000u, (unsigned long)0u,
                   (unsigned long)0u, (unsigned long)0x100u);
    check(" CFSR=01000000  MMFSR=00  BFSR=00  UFSR=0100\r\n", "cfsr");

    /* FPU S-register lines (%-2u left-aligned width 2). */
    reset();
    tracer_xprintf("  S%-2u=%08lX S%-2u=%08lX\r\n",
                   0u, (unsigned long)0x3F800000u,
                   1u, (unsigned long)0x40000000u);
    check("  S0 =3F800000 S1 =40000000\r\n", "fpu s0s1");

    reset();
    tracer_xprintf("  S%-2u=%08lX S%-2u=%08lX\r\n",
                   10u, (unsigned long)0u, 11u, (unsigned long)0u);
    check("  S10=00000000 S11=00000000\r\n", "fpu s10s11");

    /* Raw-stack hex bytes. */
    reset();
    tracer_xprintf(" %02X", (unsigned)0xAB);
    check(" AB", "hex byte");

    /* Headers / metadata. */
    reset();
    tracer_xprintf("FW     : %s\r\n", "v1.2.3");
    check("FW     : v1.2.3\r\n", "fw");

    reset();
    tracer_xprintf("Up     : %lu ms\r\n", (unsigned long)0u);
    check("Up     : 0 ms\r\n", "uptime");

    reset();
    tracer_xprintf("At     : %s:%d\r\n", "app/main.c", 42);
    check("At     : app/main.c:42\r\n", "assert line");

    /* Function trace line. */
    reset();
    tracer_xprintf("  %s %08lX  +%lu\r\n", "->",
                   (unsigned long)0x1000u, (unsigned long)5u);
    check("  -> 00001000  +5\r\n", "trace");

    /* Address / EXC_RETURN with 0x prefix. */
    reset();
    tracer_xprintf("EXC_RETURN: 0x%08lX  [%s mode, %s, %s]\r\n",
                   (unsigned long)0xFFFFFFE9u, "Thread", "MSP", "Secure");
    check("EXC_RETURN: 0xFFFFFFE9  [Thread mode, MSP, Secure]\r\n", "exc_return");

    /* %p pointers: "0x" + lowercase hex; full width on 64-bit hosts too
     * (the low 32 bits alone would read as a small address on ARM32). */
    reset();
    tracer_xprintf("p=%p\r\n", (void *)(uintptr_t)0x20001000u);
    check("p=0x20001000\r\n", "ptr 32-bit value");

    reset();
    tracer_xprintf("p=%p\r\n", (void *)(uintptr_t)0x2000A0FFu);
    check("p=0x2000a0ff\r\n", "ptr lowercase hex");

    reset();
    tracer_xprintf("p=%p\r\n",
                   (void *)(uintptr_t)0x123456789ABCDEF0ull);
    check("p=0x123456789abcdef0\r\n", "ptr 64-bit full width");

    /* %% escape: one percent sign (printf semantics). */
    reset();
    tracer_xprintf("%d%% done\r\n", 100);
    check("100% done\r\n", "percent escape");

    /* long long: %lld signed (neg magnitude), %llu max, %llX hex. */
    reset();
    tracer_xprintf("v=%lld\r\n", (long long)-1234567890123LL);
    check("v=-1234567890123\r\n", "lld negative");

    reset();
    tracer_xprintf("u=%llu\r\n",
                   (unsigned long long)0xFFFFFFFFFFFFFFFFull);
    check("u=18446744073709551615\r\n", "llu max");

    reset();
    tracer_xprintf("x=%llX\r\n",
                   (unsigned long long)0x123456789ABCDEF0ull);
    check("x=123456789ABCDEF0\r\n", "llX full 64-bit");

    /* precision: %.*s dynamic cap (a pjlib pj_str_t style (len, ptr) pair). */
    reset();
    tracer_xprintf("v=%.*s\r\n", 3, "abcdef");
    check("v=abc\r\n", "%.*s cap");

    reset();
    tracer_xprintf("v=%.*s|\r\n", 5, "ab");
    check("v=ab|\r\n", "%.*s shorter kept");

    reset();
    tracer_xprintf("v=%5.3s|\r\n", "abcdef");
    check("v=  abc|\r\n", "width + string precision");

    reset();
    tracer_xprintf("v=%.*s|\r\n", -1, "hello");   /* neg = omitted */
    check("v=hello|\r\n", "%.*s negative = none");

    /* precision on numbers: min digits (leading zeros), %.0u of 0 empty. */
    reset();
    tracer_xprintf("u=%.3u\r\n", 5u);
    check("u=005\r\n", "%.3u min digits");
    reset();
    tracer_xprintf("x=%04X|\r\n", 0x1Au);
    check("x=001A|\r\n", "%04X pad");

    reset();
    tracer_xprintf("d=%05.3d|\r\n", -42);
    check("d= -042|\r\n", "precision overrides '0' flag (width spaces)");

    reset();
    tracer_xprintf("z=[%.0u]\r\n", 0u);
    check("z=[]\r\n", "%.0u of 0 is empty");

    /* ---- extra format corners (coverage) ---- */
    reset();
    tracer_xprintf("c=%c\r\n", 'A');
    check("c=A\r\n", "%c char");

    reset();
    tracer_xprintf("[%-6s]\r\n", "ab");
    check("[ab    ]\r\n", "%-s left-align pad");

    reset();
    tracer_xprintf("abc%");
    check("abc", "trailing bare %% dropped");

    reset();
    tracer_xprintf("v=%q\r\n", 1);
    check("v=%q\r\n", "unknown conversion echoed");

    reset();
    tracer_xprintf("d=%ld\r\n", (long)-5);
    check("d=-5\r\n", "%ld negative");

    reset();
    tracer_xprintf("s=%s!\r\n", (const char *)NULL);
    check("s=(null)!\r\n", "%s NULL renders (null)");

    /* long long non-negative -> the (unsigned) branch of the %lld negation. */
    reset();
    tracer_xprintf("v=%lld\r\n", (long long)1234567890123LL);
    check("v=1234567890123\r\n", "lld positive");

    if (s_failures != 0) {
        fprintf(stderr, "tracer mini-printf test: %d FAILURE(S)\n", s_failures);
        return 1;
    }
    printf("tracer mini-printf test: all passed\n");
    return 0;
}
