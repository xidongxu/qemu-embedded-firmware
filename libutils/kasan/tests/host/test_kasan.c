/* test_kasan.c -- host unit tests for the kasan library + TLSF backend.
 *
 * #includes kasan.c, kasan_alloc_tlsf.c and tlsf.c directly with the
 * region/shadow/arena redirected to host RAM arrays, so the shadow math, the
 * live-allocation record table and the real TLSF integration can be asserted
 * without an ARM toolchain, QEMU, or -fsanitize.
 *
 * Shadow semantics under test: only the user area of a live allocation is
 * unpoisoned; block headers, free blocks and the unused arena stay poisoned.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Redirect the library's memory map to host RAM.  The shadow is inline: the
 * top 1/8 of s_region is the shadow map.  The TLSF arena sits near the front
 * of s_region (large enough for the TLSF control block + a test pool). */
#define KASAN_TEST_RETURNS
#define KASAN_ARENA_SIZE 16384u
#define KASAN_LIVE_MAX 4u
/* Small quarantine so the drain path is exercised with tiny allocations. */
#define KASAN_QUARANTINE_BYTES 128u
#define KASAN_QUARANTINE_MAX 8u

static unsigned char s_region[32768] __attribute__((aligned(8)));

#define KASAN_REGION_BASE ((uint32_t)(uintptr_t)s_region)
#define KASAN_REGION_SIZE (sizeof(s_region))
#define KASAN_ARENA_EXT   ((uint32_t)(uintptr_t)s_region + 64u)

#include "../../kasan.c"
#include "../../kasan_alloc_tlsf.c"
#include "../../../../libmem/tlsf/tlsf.c"

static int sh_at(uint32_t a) {
    uint32_t shadow_offset = 0;

    if (a < KASAN_REGION_BASE ||
        a >= KASAN_REGION_BASE + KASAN_USABLE_SIZE) {
        return -1;
    }
    shadow_offset = (uint32_t)(KASAN_SHADOW_BASE - KASAN_REGION_BASE);
    return s_region[shadow_offset + ((a - KASAN_REGION_BASE) >> 3)];
}

static void test_shadow_api(void) {
    uint32_t base = KASAN_REGION_BASE + 128u;

    kasan_init();
    assert(sh_at(base) == 0);

    kasan_poison(base, 16u);
    assert(sh_at(base) == 0xff);
    assert(sh_at(base + 8u) == 0xff);
    assert(sh_at(base + 16u) == 0);

    kasan_unpoison(base, 8u);
    assert(sh_at(base) == 0);
    assert(sh_at(base + 8u) == 0xff);

    kasan_poison(KASAN_REGION_BASE + KASAN_USABLE_SIZE + 16u, 8u);
    assert(sh_at(KASAN_REGION_BASE + KASAN_USABLE_SIZE + 16u) == -1);
}

static void test_partial_granule(void) {
    uint32_t base = KASAN_REGION_BASE + 256u;
    uint32_t before = 0;

    kasan_init();
    /* Unpoison a 20-byte range (8-aligned base): the tail granule must be
     * partial (0x04 = first 4 bytes addressable). */
    kasan_unpoison(base, 20u);
    assert(sh_at(base) == 0);
    assert(sh_at(base + 8u) == 0);
    assert(sh_at(base + 16u) == 0x04);
    assert(sh_at(base + 24u) == 0);

    /* In-bounds access (last byte of the partial tail) passes. */
    before = kasan_reports;
    __asan_load1_noabort(base + 19u);
    assert(kasan_reports == before);

    /* A 1-byte store at offset 20 hits the poisoned rest of the granule. */
    __asan_store1_noabort(base + 20u);
    assert(kasan_reports == before + 1u);
    assert(kasan_report_type == 2u);
    assert(kasan_report_pc != 0);   /* faulting PC captured at the hook level */

    /* A 4-byte load straddling the boundary also faults. */
    before = kasan_reports;
    __asan_load4_noabort(base + 17u);
    assert(kasan_reports == before + 1u);

    /* Re-poison the range: the tail granule is fully poisoned again. */
    kasan_poison(base, 20u);
    assert(sh_at(base + 16u) == 0xff);
}

static void test_heap_layout(void) {
    uint8_t *p = 0;
    uint32_t up = 0;

    kasan_heap_init();
    /* The TLSF control block at the arena start is redzone. */
    assert(sh_at(KASAN_ARENA_EXT) == 0xFB);

    p = (uint8_t *)kasan_malloc(32u);
    assert(p != 0);
    up = (uint32_t)(uintptr_t)p;
    assert((up & 7u) == 0u);            /* 8-aligned, matches shadow granule */

    assert(up >= KASAN_REGION_BASE);
    assert(sh_at(up - 8u) == 0xFB);     /* header redzone          */
    assert(sh_at(up) == 0);             /* user area unpoisoned    */
    assert(sh_at(up + 24u) == 0);
    assert(sh_at(up + 32u) == 0xFB);    /* next header redzone     */
}

static void test_heap_split_and_reuse(void) {
    uint8_t *a = 0;
    uint8_t *b = 0;
    uint32_t ba = 0;

    kasan_heap_init();
    a = (uint8_t *)kasan_malloc(32u);
    b = (uint8_t *)kasan_malloc(32u);
    assert(a != 0 && b != 0);
    assert(a != b);
    assert(((uintptr_t)a & 7u) == 0u);
    assert(((uintptr_t)b & 7u) == 0u);

    ba = (uint32_t)(uintptr_t)b;
    assert(ba > (uint32_t)(uintptr_t)a);
    assert(sh_at((uint32_t)(uintptr_t)a) == 0);
    assert(sh_at(ba - 8u) != 0);        /* b header poisoned */
}

static void test_free_poisons(void) {
    uint8_t *a = 0;
    uint32_t up = 0;

    kasan_heap_init();
    a = (uint8_t *)kasan_malloc(32u);
    up = (uint32_t)(uintptr_t)a;

    kasan_free(a);
    assert(sh_at(up) == 0xFA);          /* freed -> use-after-free would trap */
    assert(sh_at(up + 24u) == 0xFA);
}

static void test_double_free_reports(void) {
    uint8_t *a = 0;
    uint32_t before = 0;

    kasan_heap_init();
    a = (uint8_t *)kasan_malloc(32u);
    before = kasan_reports;

    kasan_free(a);
    assert(kasan_reports == before);

    kasan_free(a);                      /* double free */
    assert(kasan_reports == before + 1u);
    assert(kasan_report_type == 3u);
}

static void test_bad_free_reports(void) {
    uint32_t before = 0;
    uint32_t fake = KASAN_REGION_BASE + 512u;

    kasan_heap_init();
    before = kasan_reports;
    kasan_free((void *)(uintptr_t)fake);
    assert(kasan_reports == before + 1u);
    assert(kasan_report_type == 4u);
}

static void test_edge_cases(void) {
    uint8_t *p = 0;
    uint32_t before = 0;

    kasan_heap_init();
    before = kasan_reports;
    kasan_free(0);                      /* free(NULL) is a no-op */
    assert(kasan_reports == before);

    /* TLSF rejects zero-size and wrap-around requests. */
    p = (uint8_t *)kasan_malloc(0u);
    assert(p == 0);
    p = (uint8_t *)kasan_malloc(0xFFFFFFF8u);
    assert(p == 0);

    /* Request larger than the pool is refused without corrupting the heap. */
    p = (uint8_t *)kasan_malloc(KASAN_ARENA_SIZE);
    assert(p == 0);
    p = (uint8_t *)kasan_malloc(32u);   /* heap still usable afterwards */
    assert(p != 0);
    kasan_free(p);
}

static void test_live_overflow(void) {
    uint8_t *p[5];
    uint32_t before = 0;
    uint32_t i = 0;

    kasan_heap_init();
    assert(kasan_live_overflow == 0u);

    for (i = 0; i < 5u; i++) {
        p[i] = (uint8_t *)kasan_malloc(8u);
        assert(p[i] != 0);
    }
    /* KASAN_LIVE_MAX is 4: the 5th live allocation cannot be recorded. */
    assert(kasan_live_overflow == 1u);

    before = kasan_reports;
    /* The unrecorded 5th pointer is freed best-effort (no report, no leak). */
    kasan_free(p[4]);
    assert(kasan_reports == before);
    /* Recorded pointers are still freed normally. */
    for (i = 0; i < 4u; i++) {
        kasan_free(p[i]);
    }
    assert(kasan_reports == before);
}

static void test_calloc(void) {
    uint8_t *p = 0;
    uint32_t up = 0;
    uint32_t i = 0;

    kasan_heap_init();
    p = (uint8_t *)kasan_calloc(4u, 16u);
    assert(p != 0);
    assert(((uintptr_t)p & 7u) == 0u);
    up = (uint32_t)(uintptr_t)p;
    for (i = 0; i < 64u; i++) {
        assert(p[i] == 0);
    }
    assert(sh_at(up) == 0);             /* unpoisoned */
    kasan_free(p);
    assert(sh_at(up) != 0);             /* re-poisoned */
}

static void test_memalign(void) {
    uint8_t *p = 0;
    uint32_t up = 0;

    kasan_heap_init();
    p = (uint8_t *)kasan_memalign(32u, 16u);
    assert(p != 0);
    assert(((uintptr_t)p & 31u) == 0u);
    up = (uint32_t)(uintptr_t)p;
    assert(sh_at(up) == 0);             /* unpoisoned */
    kasan_free(p);
    assert(sh_at(up) != 0);             /* re-poisoned */
}

static void test_realloc(void) {
    uint8_t *a = 0;
    uint8_t *b = 0;
    uint32_t before = 0;
    uint32_t a_up = 0;

    kasan_heap_init();

    /* Grow 16 -> 64: may stay in place or move; the old pointer must be
     * re-poisoned if moved. */
    a = (uint8_t *)kasan_malloc(16u);
    assert(a != 0);
    a[0] = 0xAB;
    a_up = (uint32_t)(uintptr_t)a;
    b = (uint8_t *)kasan_realloc(a, 64u);
    assert(b != 0);
    assert(b[0] == 0xAB);               /* content preserved */
    assert(sh_at((uint32_t)(uintptr_t)b) == 0);
    if (b == a) {
        assert(sh_at(a_up + 16u) == 0); /* grown tail unpoisoned */
    } else {
        assert(sh_at(a_up) != 0);       /* moved: old block re-poisoned */
    }

    /* realloc(NULL, n) behaves like malloc. */
    b = (uint8_t *)kasan_realloc(0, 32u);
    assert(b != 0);
    kasan_free(b);

    /* realloc(p, 0) behaves like free; a second free is a double-free. */
    b = (uint8_t *)kasan_malloc(32u);
    assert(b != 0);
    before = kasan_reports;
    assert(kasan_realloc(b, 0u) == 0);
    assert(kasan_reports == before);
    kasan_free(b);
    assert(kasan_reports == before + 1u);
    assert(kasan_report_type == 3u);

    /* realloc on a freed pointer is reported (double-free). */
    a = (uint8_t *)kasan_malloc(16u);
    kasan_free(a);
    before = kasan_reports;
    assert(kasan_realloc(a, 32u) == 0);
    assert(kasan_reports == before + 1u);
    assert(kasan_report_type == 3u);

    /* realloc on an untracked pointer is a bad-free. */
    before = kasan_reports;
    assert(kasan_realloc((void *)(uintptr_t)(KASAN_REGION_BASE + 512u),
                         32u) == 0);
    assert(kasan_reports == before + 1u);
    assert(kasan_report_type == 4u);
}

static void test_report_semantics(void) {
    uint8_t dump[KASAN_SHADOW_DUMP];
    uint8_t *p = 0;
    uint32_t up = 0;
    uint32_t before = 0;

    /* Shadow byte -> human-readable name. */
    assert(strcmp(kasan_shadow_name(0x00), "addressable") == 0);
    assert(strcmp(kasan_shadow_name(KASAN_POISON_FREED), "freed") == 0);
    assert(strcmp(kasan_shadow_name(KASAN_POISON_REDZONE), "redzone") == 0);

    kasan_heap_init();
    p = (uint8_t *)kasan_malloc(32u);
    assert(p != 0);
    up = (uint32_t)(uintptr_t)p;

    /* Shadow dump centred on the user pointer: header left, user right. */
    kasan_shadow_dump(up, dump, KASAN_SHADOW_DUMP);
    assert(dump[KASAN_SHADOW_DUMP / 2u - 1u] == KASAN_POISON_REDZONE);
    assert(dump[KASAN_SHADOW_DUMP / 2u] == 0x00);

    /* After free, the user area is 0xFA and a store is classified UAF. */
    kasan_free(p);
    assert(sh_at(up) == KASAN_POISON_FREED);
    before = kasan_reports;
    __asan_store1_noabort(up);
    assert(kasan_reports == before + 1u);
    assert(kasan_report_cause == 2u);
    assert(kasan_report_shadow == KASAN_POISON_FREED);
}

static void test_wrap_copy(void) {
    uint8_t *a = 0;
    uint32_t before = 0;

    kasan_heap_init();
    a = (uint8_t *)kasan_malloc(16u);
    assert(a != 0);

    /* memcpy reading past the allocation (src overflow) -> redzone fault. */
    before = kasan_reports;
    __wrap_memcpy(a, a, 32u);
    assert(kasan_reports == before + 1u);
    assert(kasan_report_cause == 1u);

    /* memset writing past the allocation -> redzone fault. */
    before = kasan_reports;
    __wrap_memset(a, 0, 32u);
    assert(kasan_reports == before + 1u);
    assert(kasan_report_cause == 1u);

    /* In-bounds copies pass. */
    before = kasan_reports;
    __wrap_memcpy(a, a, 8u);
    __wrap_memmove(a, a, 8u);
    __wrap_memset(a, 0, 8u);
    assert(kasan_reports == before);
}

static void test_quarantine(void) {
    uint8_t *a = 0;
    uint8_t *b = 0;
    uint8_t *c = 0;
    uint8_t *d = 0;
    uint8_t *e = 0;
    uint8_t *f = 0;
    uint32_t up = 0;
    uint32_t before = 0;

    kasan_heap_init();

    /* Freed block stays 0xFA while quarantined; a stale write is a UAF and
     * carries the alloc + free call sites. */
    a = (uint8_t *)kasan_malloc(32u);
    assert(a != 0);
    up = (uint32_t)(uintptr_t)a;
    kasan_free(a);
    assert(sh_at(up) == KASAN_POISON_FREED);
    before = kasan_reports;
    __asan_store1_noabort(up);
    assert(kasan_reports == before + 1u);
    assert(kasan_report_cause == 2u);
    assert(kasan_report_alloc_pc != 0);
    assert(kasan_report_free_pc != 0);

    /* Double-free is still caught while the record lingers. */
    before = kasan_reports;
    kasan_free(a);
    assert(kasan_reports == before + 1u);
    assert(kasan_report_type == 3u);

    /* Same-size allocations must NOT reuse the quarantined block. */
    a = (uint8_t *)kasan_malloc(32u);
    assert(a != 0);
    up = (uint32_t)(uintptr_t)a;
    kasan_free(a);
    b = (uint8_t *)kasan_malloc(32u);
    assert(b != 0);
    assert(b != a);
    kasan_free(b);
    c = (uint8_t *)kasan_malloc(32u);
    assert(c != 0);
    assert(c != a);
    kasan_free(c);
    d = (uint8_t *)kasan_malloc(32u);
    assert(d != 0);
    assert(d != a);
    kasan_free(d);
    /* Quarantine now holds a+b+c+d = 128 bytes (the cap). */
    e = (uint8_t *)kasan_malloc(32u);
    assert(e != 0);
    assert(e != a);
    kasan_free(e);   /* over the cap -> drains the oldest block (a) */

    /* a is back in the allocator: the next same-size malloc reuses it. */
    f = (uint8_t *)kasan_malloc(32u);
    assert(f == a);

    kasan_quarantine_drain();
}

static void test_global_redzone(void) {
    struct __asan_global globals[1];
    uint32_t beg = KASAN_REGION_BASE + 256u;
    uint32_t before = 0;

    kasan_init();
    globals[0].beg = (const volatile void *)(uintptr_t)beg;
    globals[0].size = 8u;
    globals[0].size_with_redzone = 32u;
    globals[0].name = "g";
    globals[0].module_name = "t";
    globals[0].has_dynamic_init = 0;
    globals[0].location = 0;
    globals[0].odr_indicator = 0;

    __asan_register_globals(globals, 1u);
    assert(sh_at(beg) == 0);                 /* object body addressable */
    assert(sh_at(beg + 8u) == KASAN_POISON_GLOBAL);
    assert(sh_at(beg + 24u) == KASAN_POISON_GLOBAL);
    assert(sh_at(beg + 32u) == 0);           /* past the redzone */

    before = kasan_reports;
    __asan_store4_noabort(beg + 8u);         /* OOB write into the redzone */
    assert(kasan_reports == before + 1u);
    assert(kasan_report_cause == 1u);
    assert(kasan_report_shadow == KASAN_POISON_GLOBAL);

    __asan_unregister_globals(globals, 1u);
    assert(sh_at(beg + 8u) == 0);            /* unregister unpoisons */
}

static void test_stack_redzone(void) {
    uint32_t base = KASAN_REGION_BASE + 384u;
    uint32_t before = 0;

    kasan_init();
    /* The compiler writes 0xF1 (stack-left redzone) for stack redzones; it
     * must be treated as FULLY poisoned, so a mid-granule access (byte 4,
     * not the "first byte") is also caught. */
    kasan_poison_as(base, 8u, KASAN_POISON_STACK_LEFT);
    assert(sh_at(base) == KASAN_POISON_STACK_LEFT);

    before = kasan_reports;
    __asan_store1_noabort(base + 4u);
    assert(kasan_reports == before + 1u);
    assert(kasan_report_cause == 1u);
    assert(kasan_report_shadow == KASAN_POISON_STACK_LEFT);
}

int main(void) {
    kasan_set_alloc_backend(kasan_tlsf_backend());
    test_shadow_api();
    test_partial_granule();
    test_heap_layout();
    test_heap_split_and_reuse();
    test_free_poisons();
    test_double_free_reports();
    test_bad_free_reports();
    test_edge_cases();
    test_live_overflow();
    test_calloc();
    test_memalign();
    test_realloc();
    test_report_semantics();
    test_wrap_copy();
    test_quarantine();
    test_global_redzone();
    test_stack_redzone();
    printf("kasan host tests: ALL PASSED (reports=%u)\n",
           (unsigned)kasan_reports);
    return 0;
}
