/* kasan_test.c -- QEMU firmware test for the kasan library.
 * Instrumented with -fsanitize=kernel-address: every memory access calls the
 * kasan __asan_*_noabort hooks.  KHEAP_CASE injects one fault; ks_report()
 * parks the info in ks_* markers then traps (gdb reads them).
 *
 *   1 = heap overflow   : write past an allocation into the neighbour's
 *                         poisoned header -> caught on the spot
 *   2 = use-after-free  : write after ks_free() (block re-poisoned)
 *   3 = double-free     : ks_free() twice (header magic check)
 */
#include "kasan.h"
#include <stdint.h>

volatile uint32_t g_sink;

int main(void)
{
    kasan_init();
    kasan_heap_init();

#if KHEAP_CASE == 1
    {
        uint8_t *a = (uint8_t *)kasan_malloc(32);
        uint8_t *b = (uint8_t *)kasan_malloc(64);
        uint32_t i = 0;
        for (i = 0; i < 32; i++) {
            a[i] = (uint8_t)i;
        }
        b[0] = 0xEE;
        a[32] = 0xAA;                       /* OOB -> hits b's header */
        g_sink = a[0] + b[0];
    }
#elif KHEAP_CASE == 2
    {
        uint8_t *a = (uint8_t *)kasan_malloc(32);
        a[0] = 0x11;
        kasan_free(a);
        a[0] = 0x22;                        /* use-after-free -> trap  */
        g_sink = a[0];
    }
#elif KHEAP_CASE == 3
    {
        uint8_t *a = (uint8_t *)kasan_malloc(32);
        kasan_free(a);
        kasan_free(a);                      /* double free -> trap     */
    }
#endif

    for (;;) {
    }
    return 0;
}
