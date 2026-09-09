/* kasan_alloc_tlsf.c -- TLSF allocator backend for kasan.
 *
 * Adapts libmem/tlsf to the kasan_alloc_backend_t interface.  The arena is a
 * static array inside the instrumented region by default (so heap overflows
 * into the pool are detected); host tests redirect it to their own RAM via
 * KASAN_ARENA_EXT / KASAN_ARENA_SIZE.
 *
 * Compiled WITHOUT -fsanitize (like kasan.c): TLSF's control block and block
 * headers stay poisoned in the shadow map, but TLSF itself reads/writes them
 * directly without consulting the shadow.
 */
#include "kasan.h"
#include "tlsf.h"

static uint8_t *kasan_tlsf_arena = 0;
static uint32_t kasan_tlsf_arena_size = 0;

#ifdef KASAN_ARENA_EXT
#define KASAN_TLSF_ARENA_PREP()                             \
    do {                                                    \
        kasan_tlsf_arena = (uint8_t *)(uintptr_t)KASAN_ARENA_EXT; \
        kasan_tlsf_arena_size = KASAN_ARENA_SIZE;           \
    } while (0)
#else
static uint8_t kasan_tlsf_arena_storage[KASAN_HEAP_SIZE]
    __attribute__((aligned(8)));
#define KASAN_TLSF_ARENA_PREP()                             \
    do {                                                    \
        kasan_tlsf_arena = kasan_tlsf_arena_storage;        \
        kasan_tlsf_arena_size = sizeof(kasan_tlsf_arena_storage); \
    } while (0)
#endif

static tlsf_t kasan_tlsf_handle = 0;

static void kasan_tlsf_init(uint32_t *base, uint32_t *size) {
    KASAN_TLSF_ARENA_PREP();
    kasan_tlsf_handle = tlsf_create_with_pool(kasan_tlsf_arena,
                                              kasan_tlsf_arena_size);
    *base = (uint32_t)(uintptr_t)kasan_tlsf_arena;
    *size = kasan_tlsf_arena_size;
}

static void *kasan_tlsf_malloc(uint32_t bytes) {
    /* 8-byte alignment matches the 8-byte shadow granule, so the block
     * header below a returned pointer stays poisoned (underflow caught). */
    return tlsf_memalign(kasan_tlsf_handle, 8u, (size_t)bytes);
}

static uint32_t kasan_tlsf_usable(void *p) {
    return (uint32_t)tlsf_block_size(p);
}

static void kasan_tlsf_free(void *p) {
    tlsf_free(kasan_tlsf_handle, p);
}

static void *kasan_tlsf_realloc(void *p, uint32_t bytes) {
    return tlsf_realloc(kasan_tlsf_handle, p, (size_t)bytes);
}

static void *kasan_tlsf_memalign(uint32_t align, uint32_t bytes) {
    return tlsf_memalign(kasan_tlsf_handle, (size_t)align, (size_t)bytes);
}

static const kasan_alloc_backend_t kasan_tlsf_backend_desc = {
    "tlsf",
    kasan_tlsf_init,
    kasan_tlsf_malloc,
    kasan_tlsf_usable,
    kasan_tlsf_free,
    kasan_tlsf_realloc,
    kasan_tlsf_memalign,
};

const kasan_alloc_backend_t *kasan_tlsf_backend(void) {
    return &kasan_tlsf_backend_desc;
}
