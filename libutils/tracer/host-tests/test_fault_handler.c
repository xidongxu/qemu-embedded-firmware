/* Host unit test that drives tracer's FAULT HANDLER directly, with the
 * Cortex-M system-control registers backed by a RAM array.
 *
 * On the target tracer_fault_handler() only runs when a real fault happens,
 * so host gcov never saw it.  Here we #include tracer.c with its MMIO
 * accessors (TRACER_READ32/READ16/WRITE32) redirected to a byte array that
 * covers 0xE000E000..0xE000F000, then fabricate exception frames /
 * EXC_RETURN values / CFSR..BFAR states and call the handler directly.  The
 * weak tracer_halt() is compiled as a plain return (TRACER_TEST_TRAP_RETURNS)
 * so every dump ends and we can assert on the emitted text.  This exercises
 * the whole decode + dump pipeline (fault naming, EXC_RETURN mode/stack,
 * register + status lines, MMFAR/BFAR [VALID], raw-stack clipping, assert
 * path, both re-entrancy guards) that the target only reaches via a fault.
 *
 * The BL/BLX stack walk itself is NOT run here (a host 64-bit stack pointer
 * cannot be walked) -- the low TRACER_STACK_TOP makes the walk exit
 * immediately; the walker is covered on 32-bit hosts (-m32 job) and by the
 * QEMU boards.
 *
 * Run:  gcc -std=c99 -Wall -Wextra -I.. host-tests/test_fault_handler.c -o t
 *       && ./t
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- MMIO backend: registers live in a RAM array (see tracer.c) ---- */
static uint8_t s_mmio[0x1000] __attribute__((aligned(8)));
#define TREG_OFF(a) ((unsigned)((uintptr_t)(a) - 0xE000E000u))
#define TRACER_READ32(a)  (*(volatile uint32_t *)(void *)(s_mmio + TREG_OFF(a)))
#define TRACER_WRITE32(a, v) \
    (*(volatile uint32_t *)(void *)(s_mmio + TREG_OFF(a)) = (uint32_t)(v))
#define TRACER_READ16(a)  (*(volatile uint16_t *)(void *)(s_mmio + TREG_OFF(a)))

static char s_out[16384];
static size_t s_len = 0;
static void putc_out(int c) {
    if (s_len + 1u < sizeof(s_out)) {
        s_out[s_len++] = (char)c;
    }
}

/* Collected output is routed through tracer's mini-printf. */
#define TRACER_PUTCHAR putc_out
/* Let the dump handlers return to the caller instead of trapping. */
#define TRACER_TEST_TRAP_RETURNS 1
#define TRACER_USE_CRASH 1
/* Fake memory-region bounds: low enough that no host 64-bit stack/frame
 * pointer is ever walked (the scan exits immediately). */
#define TRACER_STACK_BASE 0x00000800u
#define TRACER_STACK_TOP  0x00001000u
#define TRACER_TEXT_START 0x00000100u
#define TRACER_TEXT_END   0x00000200u

#include "../tracer.c"

static int s_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_fail++;                                                        \
        }                                                                    \
    } while (0)

static void reg_zero(void) {
    memset(s_mmio, 0, sizeof(s_mmio));
}
static void set32(uint32_t addr, uint32_t v) {
    *(volatile uint32_t *)(void *)(s_mmio + TREG_OFF(addr)) = v;
}
static void set16(uint32_t addr, uint16_t v) {
    *(volatile uint16_t *)(void *)(s_mmio + TREG_OFF(addr)) = v;
}

static void out_reset(void) {
    s_len = 0u;
    s_out[0] = '\0';
}
static void expect(const char *needle) {
    s_out[s_len] = '\0';
    if (strstr(s_out, needle) == NULL) {
        fprintf(stderr, "FAIL expect: missing '%s'\n---- output ----\n%s\n",
                needle, s_out);
        s_fail++;
    }
}
static void not_expect(const char *needle) {
    s_out[s_len] = '\0';
    if (strstr(s_out, needle) != NULL) {
        fprintf(stderr, "FAIL not-expect: found '%s'\n", needle);
        s_fail++;
    }
}

/* ---- fabricate an exception frame + run the handler ---- */
typedef struct {
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
} frame_t;
typedef struct {
    uint32_t r4, r5, r6, r7, r8, r9, r10, r11;
} core_t;
static frame_t g_fr;
static core_t  g_co;

/* Reset the tracer dump state + MMIO + output, then fill a plausible frame. */
static void prep_fault(uint32_t xpsr) {
    s_tracer_dumping = 0u;   /* static in the included tracer.c */
    reg_zero();
    out_reset();
    g_fr.r0 = 0x10u; g_fr.r1 = 0x11u; g_fr.r2 = 0x12u; g_fr.r3 = 0x13u;
    g_fr.r12 = 0x20u; g_fr.lr = 0x1000u; g_fr.pc = 0x2000u; g_fr.xpsr = xpsr;
    g_co.r4 = 4u; g_co.r5 = 5u; g_co.r6 = 6u; g_co.r7 = 7u;
    g_co.r8 = 8u; g_co.r9 = 9u; g_co.r10 = 10u; g_co.r11 = 11u;
}
static void fire(uint32_t exc_return) {
    tracer_fault_handler((uint32_t *)&g_fr, exc_return, (uint32_t *)&g_co);
}

int main(void) {
    /* 1. BusFault, precise + BFARVALID, Thread/PSP -> BFAR tagged valid. */
    prep_fault(0u);                       /* IPSR=0 */
    set32(0xE000ED28u, 0x00008200u);      /* CFSR: PRECISERR | BFARVALID */
    set32(0xE000ED38u, 0xDEADBEEFu);      /* BFAR */
    set16(0xE000ED2Au, 0x0000u);          /* UFSR */
    fire(0xFFFFFFFDu);                    /* Thread mode, PSP */
    expect("BusFault Fault Dump");
    expect("Thread mode, PSP");
    expect("BFAR=DEADBEEF [VALID]");
    expect("MMFAR=00000000");

    /* 2. MemManage, MMARVALID, Handler/MSP -> MMFAR tagged; IPSR=4. */
    prep_fault(4u);                       /* IPSR = MemManage */
    set32(0xE000ED28u, 0x00000081u);      /* CFSR: IACCVIOL | MMARVALID */
    set32(0xE000ED34u, 0x20004000u);      /* MMFAR */
    fire(0xFFFFFFF1u);                    /* Handler mode, MSP */
    expect("MemManage Fault Dump");
    expect("Handler mode, MSP");
    expect("Exception : MemManage (IPSR=4)");
    expect("MMFAR=20004000 [VALID]");

    /* 3. UsageFault via the 16-bit UFSR (thread/MSP). */
    prep_fault(0u);
    set16(0xE000ED2Au, 0x0001u);          /* UFSR: UNDEFINSTR */
    fire(0xFFFFFFE9u);                    /* Thread mode, MSP */
    expect("UsageFault Fault Dump");
    expect("Thread mode, MSP");
    expect("UFSR=0001");

    /* 4. HardFault only (HFSR.FORCED), no CFSR bits. */
    prep_fault(0u);
    set32(0xE000ED2Cu, 0x40000000u);      /* HFSR: FORCED */
    fire(0xFFFFFFE9u);
    expect("HardFault Fault Dump");
    expect("HFSR=40000000");

    /* 5. no status bits at all -> decode from IPSR (NMI, ipsr=2). */
    prep_fault(2u);                       /* IPSR = NMI */
    fire(0xFFFFFFFDu);
    expect("NMI Fault Dump");
    expect("Exception : NMI (IPSR=2)");

    /* 6. BFAR present but NOT valid -> no [VALID] tag. */
    prep_fault(0u);
    set32(0xE000ED28u, 0x00000200u);      /* CFSR: PRECISERR only */
    set32(0xE000ED38u, 0x12345678u);
    fire(0xFFFFFFFDu);
    expect("BFAR=12345678");
    not_expect("BFAR=12345678 [VALID]");

    /* 6b. NonSecure EXC_RETURN (bit0=0) branch of the mode line. */
    prep_fault(0u);
    fire(0xFFFFFFE8u);                    /* Thread, MSP, NonSecure */
    expect("Thread mode, MSP, NonSecure");

    /* 7. end-of-dump trap line is reached (handler returns via test trap). */
    expect("===== End of dump (trapped) =====");

    /* 8. assert: expression + file:line + callstack + trap line. */
    s_tracer_dumping = 0u;
    out_reset();
    tracer_assert_fail("x == 0", "app/main.c", 42);
    expect("Tracer: Assert Failed");
    expect("Assert : x == 0");
    expect("At     : app/main.c:42");
    expect("===== End of assert (trapped) =====");

    /* 9. assert with NULL expression / file. */
    s_tracer_dumping = 0u;
    out_reset();
    tracer_assert_fail(NULL, NULL, 0);
    expect("Assert : ?");
    expect("At     : ?:0");

    /* 10. re-entrant fault (dump already in progress) -> guard, no recurse. */
    s_tracer_dumping = 1u;
    out_reset();
    tracer_fault_handler((uint32_t *)&g_fr, 0xFFFFFFFDu, (uint32_t *)&g_co);
    expect("fault while dumping, ignored");

    /* 11. re-entrant assert -> its own guard. */
    s_tracer_dumping = 1u;
    out_reset();
    tracer_assert_fail("x", "f", 1);
    expect("assert while dumping, ignored");

    /* 12. on-demand snapshot (dump_all) still returns. */
    s_tracer_dumping = 0u;
    out_reset();
    tracer_dump_all();
    expect("on-demand snapshot");
    expect("End of snapshot");

    if (s_fail != 0) {
        fprintf(stderr, "fault-handler host test: %d FAILURE(S)\n", s_fail);
        return 1;
    }
    printf("fault-handler host test: all passed\n");
    return 0;
}
