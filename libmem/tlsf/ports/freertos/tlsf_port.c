/**
 * @file  tlsf_port.c
 * @brief Unified TLSF allocator integration layer (RTOS port, implementation).
 *
 * Backs the C library malloc/free/calloc/realloc (via linker --wrap), C++
 * new/delete and the FreeRTOS kernel heap (pvPortMalloc/pvPortFree) with a
 * single TLSF pool, so every allocation in the project shares one allocator
 * (low fragmentation, O(1) allocation, unified stats).
 *
 * Pool: the linker-defined libc heap region [_end, _estack - _Min_Stack_Size).
 * Lazy-initialised on the first call, so it works before the FreeRTOS
 * scheduler starts (single-threaded at that point).
 *
 * Locking: FreeRTOS critical section (interrupts off) around every operation.
 */
#include <string.h>

#include "tlsf_port.h"
#include "tlsf.h"

#include "FreeRTOS.h"
#include "task.h"

/* Linker symbols describing the heap region (see gcc_arm.ld). */
extern uint8_t _end;
extern uint8_t _estack;
extern uint32_t _Min_Stack_Size;

/* TLSF instance (single shared allocator). */
static tlsf_t g_tlsf;
static int    g_tlsf_inited;

/* Low-water mark of free memory (updated on stats query). */
static size_t g_min_free;

/* Total pool size (set at init). */
static size_t g_pool_bytes;

static void tlsf_port_lock(void) {
    portENTER_CRITICAL();
}

static void tlsf_port_unlock(void) {
    portEXIT_CRITICAL();
}

static void tlsf_port_init(void) {
    uint32_t start = 0;
    uint32_t limit = 0;
    size_t bytes = 0;
    tlsf_t t = NULL;

    if (g_tlsf_inited) {
        return;
    }

    start = (uint32_t)&_end;
    limit = (uint32_t)&_estack - (uint32_t)&_Min_Stack_Size;

    if (limit <= start) {
        g_tlsf_inited = 1;
        return;
    }
    bytes = (size_t)(limit - start);

    t = tlsf_create_with_pool((void *)start, bytes);
    if (t != NULL) {
        g_tlsf = t;
        g_min_free = bytes;
        g_pool_bytes = bytes;
    }
    g_tlsf_inited = 1;
}

/* Stats (walk the pool; used on low-frequency queries only). */
typedef struct {
    size_t used;
    size_t free;
} tlsf_port_stats_t;

static void tlsf_port_walker(void *ptr, size_t size, int used, void *user) {
    tlsf_port_stats_t *s = (tlsf_port_stats_t *)user;
    (void)ptr;
    if (used) {
        s->used += size;
    } else {
        s->free += size;
    }
}

void *tlsf_port_malloc(size_t n) {
    void *p = NULL;

    tlsf_port_lock();
    tlsf_port_init();
    p = g_tlsf ? tlsf_malloc(g_tlsf, n) : NULL;
    tlsf_port_unlock();
    return p;
}

void *tlsf_port_calloc(size_t nmemb, size_t size) {
    void *p = NULL;
    size_t total = 0;

    if (size != 0 && nmemb > (size_t)-1 / size) {
        return NULL;
    }
    total = nmemb * size;

    tlsf_port_lock();
    tlsf_port_init();
    p = g_tlsf ? tlsf_malloc(g_tlsf, total) : NULL;
    if (p != NULL) {
        memset(p, 0, total);
    }
    tlsf_port_unlock();
    return p;
}

void *tlsf_port_realloc(void *ptr, size_t size) {
    void *p = NULL;

    if (ptr == NULL) {
        return tlsf_port_malloc(size);
    }
    if (size == 0) {
        tlsf_port_free(ptr);
        return NULL;
    }

    tlsf_port_lock();
    tlsf_port_init();
    p = g_tlsf ? tlsf_realloc(g_tlsf, ptr, size) : NULL;
    tlsf_port_unlock();
    return p;
}

void *tlsf_port_memalign(size_t align, size_t size) {
    void *p = NULL;

    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }
    if ((align & (align - 1)) != 0) {
        return NULL;
    }

    tlsf_port_lock();
    tlsf_port_init();
    p = g_tlsf ? tlsf_memalign(g_tlsf, align, size) : NULL;
    tlsf_port_unlock();
    return p;
}

void tlsf_port_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    tlsf_port_lock();
    tlsf_port_init();
    if (g_tlsf) {
        tlsf_free(g_tlsf, ptr);
    }
    tlsf_port_unlock();
}

size_t tlsf_port_get_free_size(void) {
    tlsf_port_stats_t s = {0, 0};
    size_t free_bytes = 0;

    memset(&s, 0, sizeof(s));

    tlsf_port_lock();
    tlsf_port_init();
    if (g_tlsf) {
        tlsf_walk_pool(tlsf_get_pool(g_tlsf), tlsf_port_walker, &s);
    }
    free_bytes = s.free;
    if (free_bytes < g_min_free) {
        g_min_free = free_bytes;
    }
    tlsf_port_unlock();
    return free_bytes;
}

size_t tlsf_port_get_min_free_size(void) {
    (void)tlsf_port_get_free_size();
    return g_min_free;
}

size_t tlsf_port_get_used_size(void) {
    tlsf_port_stats_t s = {0, 0};
    size_t used_bytes = 0;

    memset(&s, 0, sizeof(s));

    tlsf_port_lock();
    tlsf_port_init();
    if (g_tlsf) {
        tlsf_walk_pool(tlsf_get_pool(g_tlsf), tlsf_port_walker, &s);
    }
    used_bytes = s.used;
    tlsf_port_unlock();
    return used_bytes;
}

size_t tlsf_port_get_total_size(void) {
    tlsf_port_init();
    return g_pool_bytes;
}

/* C library hooks (via linker --wrap=malloc,free,calloc,realloc,memalign). */
void *__wrap_malloc(size_t size) {
    return tlsf_port_malloc(size);
}

void __wrap_free(void *ptr) {
    tlsf_port_free(ptr);
}

void *__wrap_calloc(size_t nmemb, size_t size) {
    return tlsf_port_calloc(nmemb, size);
}

void *__wrap_realloc(void *ptr, size_t size) {
    return tlsf_port_realloc(ptr, size);
}

void *__wrap_memalign(size_t align, size_t size) {
    return tlsf_port_memalign(align, size);
}
