/**
 ******************************************************************************
 * @file      sysmem_tlsf.c
 * @brief     newlib _sbrk backing pool for the TLSF unified-allocator mode.
 *
 *            When the project is built with the unified TLSF allocator
 *            (TLSF_MALLOC=ON, see libmem/tlsf), the linker --wrap routes
 *            malloc/free/calloc/realloc and C++ new/delete through TLSF, and
 *            the FreeRTOS kernel heap (pvPortMalloc) uses the same pool.
 *
 *            newlib's OWN internal malloc calls (some libc internals) are NOT
 *            routed through TLSF, so they still call _sbrk().  This file gives
 *            _sbrk a small, separate static pool so it never overlaps the TLSF
 *            region [_end, _estack - _Min_Stack_Size) which TLSF manages.
 *
 *            Selected instead of sysmem.c in the FreeRTOS build when
 *            TLSF_MALLOC is ON; sysmem.c stays as the stock CubeMX version for
 *            the non-TLSF builds.
 ******************************************************************************
 */

/* Includes */
#include <errno.h>
#include <stdint.h>

/**
 * Pointer to the current high watermark of the libc backing pool
 */
static uint8_t *__sbrk_heap_end = NULL;

/**
 * Small, dedicated backing pool for newlib's internal allocations.
 * 32 KiB is plenty for nano-libc temporaries; the main heap is TLSF's.
 */
static uint8_t __attribute__((aligned(8))) sbrk_pool[32 * 1024];

/**
 * @brief _sbrk() allocates memory to the newlib heap (TLSF mode)
 *
 * The main heap region ([_end, _estack - _Min_Stack_Size)) is owned by the
 * unified TLSF allocator (libmem/tlsf).  newlib's internal malloc calls are
 * not routed through TLSF, so this _sbrk serves them from a separate static
 * pool to avoid double-managing the TLSF region.
 *
 * @param incr Memory size
 * @return Pointer to allocated memory
 */
void *_sbrk(ptrdiff_t incr) {
  uint8_t *prev_heap_end = NULL;
  uint8_t *pool_end = sbrk_pool + sizeof(sbrk_pool);

  /* Initialize heap end at first call */
  if (NULL == __sbrk_heap_end) {
    __sbrk_heap_end = sbrk_pool;
  }

  /* Protect the small backing pool from overflow */
  if (__sbrk_heap_end + incr > pool_end) {
    errno = ENOMEM;
    return (void *)-1;
  }

  prev_heap_end = __sbrk_heap_end;
  __sbrk_heap_end += incr;

  return (void *)prev_heap_end;
}
