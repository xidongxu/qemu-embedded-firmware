/*
 * @file  tlsf_freertos.c
 * @brief FreeRTOS kernel heap implementation backed by the unified TLSF
 *        allocator (libmem/tlsf).
 *
 * Built by the FreeRTOS build when FREERTOS_HEAP is set to this file.  It
 * routes pvPortMalloc/pvPortFree (task stacks, TCBs, queues, ...) through the
 * same TLSF pool used by the C library malloc/new, so the whole project
 * shares one allocator and one set of memory stats.
 *
 * The tlsf_port_* symbols are provided by the TLSF integration layer
 * (libmem/tlsf/ports/freertos/tlsf_port.c), linked separately.
 */
#include "FreeRTOS.h"
#include "task.h"

/* Provided by the TLSF integration layer (declared, not included - this file
 * is compiled by the FreeRTOS build which has no libmem/tlsf include dir). */
void *tlsf_port_malloc(size_t n);
void tlsf_port_free(void *p);
size_t tlsf_port_get_free_size(void);
size_t tlsf_port_get_min_free_size(void);
size_t tlsf_port_get_used_size(void);

void *pvPortMalloc(size_t xWantedSize) {
    void *pvReturn = tlsf_port_malloc(xWantedSize);

    if ((pvReturn == NULL) && (configUSE_MALLOC_FAILED_HOOK == 1)) {
        vApplicationMallocFailedHook();
    }
    return pvReturn;
}

void vPortFree(void *pv) {
    tlsf_port_free(pv);
}

size_t xPortGetFreeHeapSize(void) {
    return tlsf_port_get_free_size();
}

size_t xPortGetMinimumEverFreeHeapSize(void) {
    return tlsf_port_get_min_free_size();
}

#if ( configUSE_HEAP_STATS == 1 )

void vApplicationGetHeapStats(HeapStats_t *pxHeapStats) {
    (void)pxHeapStats;
}

#endif /* configUSE_HEAP_STATS */
