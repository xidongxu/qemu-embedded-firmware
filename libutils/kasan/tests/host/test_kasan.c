/* test_kasan.c -- host unit tests for the kasan library.
 *
 * #includes kasan.c directly with the region/shadow/arena redirected to host
 * RAM arrays, so the shadow math and the first-fit heap can be asserted
 * without an ARM toolchain, QEMU, or -fsanitize.
 *
 * Shadow semantics under test: only the user area of a live allocation is
 * unpoisoned; headers, free blocks and unused arena stay poisoned.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Redirect the library's memory map to host RAM.  The shadow is inline: the
 * top 1/8 of s_region (its last 128 bytes) is the shadow map. */
#define KASAN_TEST_RETURNS
#define KASAN_HEAP_SIZE 256u

static unsigned char s_region[1024] __attribute__((aligned(8)));

#define KASAN_REGION_BASE ((uint32_t)(uintptr_t)s_region)
#define KASAN_REGION_SIZE (sizeof(s_region))
#define KASAN_ARENA_EXT   ((uint32_t)(uintptr_t)s_region + 64u)

#include "../../kasan.c"

static int sh_at(uint32_t a)
{
    uint32_t shadow_offset = 0;

    if (a < KASAN_REGION_BASE ||
        a >= KASAN_REGION_BASE + KASAN_USABLE_SIZE) {
        return -1;
    }
    shadow_offset = (uint32_t)(KASAN_SHADOW_BASE - KASAN_REGION_BASE);
    return s_region[shadow_offset + ((a - KASAN_REGION_BASE) >> 3)];
}

static void test_shadow_api(void)
{
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

static void test_heap_layout(void)
{
    uint8_t *p = 0;
    uint32_t up = 0;

    kasan_heap_init();
    assert(sh_at((uint32_t)(uintptr_t)kasan_arena) != 0);

    p = (uint8_t *)kasan_malloc(32u);
    assert(p != 0);
    up = (uint32_t)(uintptr_t)p;

    assert(up >= KASAN_REGION_BASE);
    assert(sh_at(up - 8u) != 0);        /* header poisoned  */
    assert(sh_at(up) == 0);             /* user area free   */
    assert(sh_at(up + 24u) == 0);
    assert(sh_at(up + 32u) != 0);       /* neighbour (or free) poisoned */
}

static void test_heap_split_and_reuse(void)
{
    uint8_t *a = 0;
    uint8_t *b = 0;
    uint32_t ba = 0;

    a = (uint8_t *)kasan_malloc(32u);
    b = (uint8_t *)kasan_malloc(32u);
    assert(a != 0 && b != 0);
    assert(a != b);

    ba = (uint32_t)(uintptr_t)b;
    assert(ba > (uint32_t)(uintptr_t)a);
    assert(sh_at((uint32_t)(uintptr_t)a) == 0);
    assert(sh_at(ba - 8u) != 0);        /* b header is poisoned */
}

static void test_free_poisons(void)
{
    uint8_t *a = (uint8_t *)kasan_malloc(32u);
    uint32_t up = (uint32_t)(uintptr_t)a;

    kasan_free(a);
    assert(sh_at(up) != 0);             /* use-after-free would trap */
    assert(sh_at(up + 24u) != 0);
}

static void test_double_free_reports(void)
{
    uint8_t *a = (uint8_t *)kasan_malloc(32u);
    uint32_t before = kasan_reports;

    kasan_free(a);
    assert(kasan_reports == before);

    kasan_free(a);                      /* double free */
    assert(kasan_reports == before + 1u);
    assert(kasan_report_type == 3u);
}

static void test_bad_free_reports(void)
{
    uint32_t before = kasan_reports;
    uint32_t fake = KASAN_REGION_BASE + 512u;

    kasan_free((void *)(uintptr_t)fake);
    assert(kasan_reports == before + 1u);
    assert(kasan_report_type == 4u);
}

int main(void)
{
    test_shadow_api();
    test_heap_layout();
    test_heap_split_and_reuse();
    test_free_poisons();
    test_double_free_reports();
    test_bad_free_reports();
    printf("kasan host tests: ALL PASSED (reports=%u)\n",
           (unsigned)kasan_reports);
    return 0;
}
