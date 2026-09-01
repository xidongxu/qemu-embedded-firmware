/* Host unit test for the pure logic in libutils/tracer.
 *
 * Compiles tracer.c directly on the HOST (no ARM toolchain, no firmware) so
 * the static helpers can be exercised: Thumb-2 BL/BLX return-address
 * detection, exception/fault-name decoding, byte-wise 32-bit loads and the
 * stack-scan walker.  Run it wherever you build:
 *
 *   gcc -std=c99 -Wall -Wextra -I.. tests/test_tracer.c -o /tmp/test_tracer
 *   /tmp/test_tracer
 *
 * Also wired into CTest via -DTRACER_BUILD_TESTS=ON (tests/CMakeLists.txt).
 *
 * Pointer-based helpers (tracer_load32, tracer_walk_callstack) pass memory
 * addresses through uint32_t, so they only work on 32-bit hosts; on 64-bit
 * hosts those cases are skipped (they are covered by the on-device/QEMU
 * integration tests anyway).
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Route tracer output nowhere and disable the firmware-only raw-stack dump
 * before pulling tracer.c in.  Provide a fake .text region so the stack
 * walker has a mapped "code" segment to read instructions from. */
#define TRACER_PRINTF(...) ((void)0)
#define TRACER_STACK_DUMP_BYTES 0u

static uint8_t s_fake_text[64]; /* fake .text the walker may read from */
#define TRACER_TEXT_START ((uint32_t)(uintptr_t)s_fake_text)
#define TRACER_TEXT_END   ((uint32_t)(uintptr_t)(s_fake_text + sizeof(s_fake_text)))

#include "../tracer.c"

static int s_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failures++;                                                     \
        }                                                                     \
    } while (0)

/* ---- Thumb-2 BL/BLX decode ------------------------------------------- */

static void test_bl_blx(void) {
    /* BL  = 11110 S imm10 | 11 J1 1 J2 imm11 ; only the FIXED bits matter
     * (J1/J2 encode the target and must not be part of the match). */
    uint32_t bl_j1_0 = 0xF000u | (0xD800u << 16);  /* J1=0 J2=0 (old code missed) */
    uint32_t bl_j1_1 = 0xF000u | ((0xD800u | 0x2000u) << 16); /* J1=1 */
    uint32_t blx     = 0xF000u | (0xD000u << 16);
    uint32_t not_hw1 = 0x4800u | (0xD800u << 16); /* halfword1 != 11110 */
    uint32_t wrong_h = 0xE000u | (0xD800u << 16); /* 11100 = B.W, not BL */
    uint32_t wrong_l = 0xF000u | (0xC800u << 16); /* halfword2 masked != D800/D000 */

    CHECK(tracer_is_bl_or_blx(bl_j1_0) == 1);
    CHECK(tracer_is_bl_or_blx(bl_j1_1) == 1);
    CHECK(tracer_is_bl_or_blx(blx) == 1);
    CHECK(tracer_is_bl_or_blx(not_hw1) == 0);
    CHECK(tracer_is_bl_or_blx(wrong_h) == 0);
    CHECK(tracer_is_bl_or_blx(wrong_l) == 0);
}

/* ---- Exception / fault name decoding --------------------------------- */

static void test_names(void) {
    CHECK(strcmp(tracer_exc_name(2), "NMI") == 0);
    CHECK(strcmp(tracer_exc_name(3), "HardFault") == 0);
    CHECK(strcmp(tracer_exc_name(4), "MemManage") == 0);
    CHECK(strcmp(tracer_exc_name(5), "BusFault") == 0);
    CHECK(strcmp(tracer_exc_name(6), "UsageFault") == 0);
    CHECK(strcmp(tracer_exc_name(15), "SysTick") == 0);
    CHECK(strcmp(tracer_exc_name(16), "IRQn") == 0);
    CHECK(strcmp(tracer_exc_name(0), "IRQn") == 0);

    tracer_fault_t f = {0};
    f.mmfsr = 1u;  CHECK(strcmp(tracer_fault_name(&f), "MemManage") == 0);
    f.mmfsr = 0u;  f.bfsr = 1u;  CHECK(strcmp(tracer_fault_name(&f), "BusFault") == 0);
    f.bfsr = 0u;   f.ufsr = 1u;  CHECK(strcmp(tracer_fault_name(&f), "UsageFault") == 0);
    f.ufsr = 0u;   f.hfsr = 1u;  CHECK(strcmp(tracer_fault_name(&f), "HardFault") == 0);
    f.hfsr = 0u;   f.ipsr = 3u;  CHECK(strcmp(tracer_fault_name(&f), "HardFault") == 0);
    f.ipsr = 6u;   CHECK(strcmp(tracer_fault_name(&f), "UsageFault") == 0);
}

/* ---- Byte-wise load32 (32-bit hosts only) ---------------------------- */

static void test_load32(void) {
#if UINTPTR_MAX <= 0xFFFFFFFFu
    uint8_t bytes[4] = {0x12, 0x34, 0x56, 0x78};
    CHECK(tracer_load32((uint32_t)(uintptr_t)bytes) == 0x78563412u);
#else
    (void)0;
#endif
}

/* ---- Stack walker (32-bit hosts only) -------------------------------- */

static void test_walker(void) {
#if UINTPTR_MAX <= 0xFFFFFFFFu
    static uint32_t stack[16];
    uint32_t buf[8];
    uint32_t pc_base = (uint32_t)(uintptr_t)s_fake_text;
    uint32_t bl;
    uint32_t n;

    memset(stack, 0, sizeof(stack));
    memset(s_fake_text, 0, sizeof(s_fake_text));

    /* Put a real Thumb-2 BL at fake_text+0x10 (J1=J2=0). */
    bl = 0xF000u | (0xD800u << 16);
    memcpy(s_fake_text + 0x10, &bl, 4);

    /* stack[4] = return address of that BL: ret = (pc + 4) | 1 (Thumb). */
    stack[4] = pc_base + 0x10u + 4u + 1u;

    n = tracer_walk_callstack((uint32_t)(uintptr_t)stack,
                              (uint32_t)(uintptr_t)(stack + 16),
                              buf, 8);
    CHECK(n >= 1u);
    CHECK(buf[0] == pc_base + 0x10u);
#else
    (void)0;
#endif
}

int main(void) {
    test_bl_blx();
    test_names();
    test_load32();
    test_walker();

    if (s_failures != 0) {
        fprintf(stderr, "tracer host test: %d FAILURE(S)\n", s_failures);
        return 1;
    }
    printf("tracer host test: all passed\n");
    return 0;
}
