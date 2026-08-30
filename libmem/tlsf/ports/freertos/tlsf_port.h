/**
 * @file  tlsf_port.h
 * @brief Unified TLSF allocator integration layer (RTOS port API).
 *
 * All project memory (C library malloc/free/calloc/realloc, C++ new/delete,
 * and FreeRTOS kernel pvPortMalloc/pvPortFree) is routed through a single
 * TLSF pool (libmem/tlsf).  This header exposes the port-facing API used
 * by the FreeRTOS heap shim (tlsf_freertos.c) and the C++ new/delete
 * overrides (tlsf_heap.cpp).
 *
 * Thread-safety: the allocator is guarded with the FreeRTOS critical section
 * (interrupts disabled) - TLSF ops are O(1) so the critical section is tiny.
 */
#ifndef TLSF_PORT_H
#define TLSF_PORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Allocate n bytes from the unified TLSF pool. */
void *tlsf_port_malloc(size_t n);
/** Allocate zeroed memory (nmemb * size) from the unified TLSF pool. */
void *tlsf_port_calloc(size_t nmemb, size_t size);
/** Resize an allocation (NULL -> malloc, size 0 -> free). */
void *tlsf_port_realloc(void *ptr, size_t size);
/** Allocate with alignment (align is a power of two, >= sizeof(void*)). */
void *tlsf_port_memalign(size_t align, size_t size);
/** Free a pointer returned by any tlsf_port_* allocator. */
void tlsf_port_free(void *ptr);
/** Total free bytes currently available in the TLSF pool. */
size_t tlsf_port_get_free_size(void);
/** Minimum free bytes ever observed (low-water mark). */
size_t tlsf_port_get_min_free_size(void);
/** Bytes currently in use (excludes allocator overhead). */
size_t tlsf_port_get_used_size(void);
/** Total size of the TLSF pool (configured at init). */
size_t tlsf_port_get_total_size(void);

#ifdef __cplusplus
}
#endif

#endif /* TLSF_PORT_H */
