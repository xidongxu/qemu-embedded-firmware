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

static tlsf_t kasan_tlsf_default_handle = 0;

static void kasan_tlsf_init(void *ctx, uint32_t *base, uint32_t *size) {
    tlsf_t *handle = (tlsf_t *)ctx;

    KASAN_TLSF_ARENA_PREP();
    *handle = tlsf_create_with_pool(kasan_tlsf_arena,
                                    kasan_tlsf_arena_size);
    *base = (uint32_t)(uintptr_t)kasan_tlsf_arena;
    *size = kasan_tlsf_arena_size;
}

static int kasan_tlsf_init_pool(void *ctx, void *pool, uint32_t pool_size) {
    tlsf_t *handle = (tlsf_t *)ctx;

    *handle = tlsf_create_with_pool(pool, (size_t)pool_size);
    return (*handle == 0) ? -1 : 0;
}

static void *kasan_tlsf_malloc(void *ctx, uint32_t bytes) {
    tlsf_t tlsf = *(tlsf_t *)ctx;

    /* 8-byte alignment matches the 8-byte shadow granule, so the block
     * header below a returned pointer stays poisoned (underflow caught). */
    return tlsf_memalign(tlsf, 8u, (size_t)bytes);
}

static uint32_t kasan_tlsf_usable(void *ctx, void *p) {
    (void)ctx;
    return (uint32_t)tlsf_block_size(p);
}

static void kasan_tlsf_free(void *ctx, void *p) {
    tlsf_t tlsf = *(tlsf_t *)ctx;

    tlsf_free(tlsf, p);
}

static void *kasan_tlsf_realloc(void *ctx, void *p, uint32_t bytes) {
    tlsf_t tlsf = *(tlsf_t *)ctx;
    uint32_t old = (uint32_t)tlsf_block_size(p);
    uint32_t copy = (old < bytes) ? old : bytes;
    void *np = tlsf_memalign(tlsf, 8u, (size_t)bytes);
    uint8_t *d = 0;
    const uint8_t *s = (const uint8_t *)p;
    uint32_t i = 0;

    if (np == 0) {
        return 0;
    }
    /* Raw copy instead of tlsf_realloc(): the freshly allocated block is
     * still poisoned in the shadow map, and tlsf_realloc()'s internal libc
     * memcpy would be routed through __wrap_memcpy (false positive).  This
     * file is not instrumented, so the raw loop does no shadow checks. */
    d = (uint8_t *)np;
    for (i = 0; i < copy; i++) {
        d[i] = s[i];
    }
    tlsf_free(tlsf, p);
    return np;
}

static void *kasan_tlsf_memalign(void *ctx, uint32_t align, uint32_t bytes) {
    tlsf_t tlsf = *(tlsf_t *)ctx;

    return tlsf_memalign(tlsf, (size_t)align, (size_t)bytes);
}

static const kasan_alloc_backend_t kasan_tlsf_backend_desc = {
    "tlsf",
    &kasan_tlsf_default_handle,
    kasan_tlsf_init,
    kasan_tlsf_init_pool,
    kasan_tlsf_malloc,
    kasan_tlsf_usable,
    kasan_tlsf_free,
    kasan_tlsf_realloc,
    kasan_tlsf_memalign,
};

const kasan_alloc_backend_t *kasan_tlsf_backend(void) {
    return &kasan_tlsf_backend_desc;
}

/* Multi-heap factory: each call returns an independent TLSF instance whose
 * ctx points at its own handle.  kasan_heap_register() drives the actual
 * pool setup via init_pool().  The instance descriptors live in a static
 * array (no allocation needed). */
#define KASAN_TLSF_MAX_INSTANCES 8u

typedef struct {
    kasan_alloc_backend_t api;
    tlsf_t handle;
} kasan_tlsf_instance_t;

static kasan_tlsf_instance_t kasan_tlsf_instances[KASAN_TLSF_MAX_INSTANCES];

const kasan_alloc_backend_t *kasan_tlsf_create(void) {
    uint32_t i = 0;

    for (i = 0; i < KASAN_TLSF_MAX_INSTANCES; i++) {
        if (kasan_tlsf_instances[i].api.ctx == 0) {
            kasan_tlsf_instances[i].api = kasan_tlsf_backend_desc;
            kasan_tlsf_instances[i].api.ctx =
                &kasan_tlsf_instances[i].handle;
            kasan_tlsf_instances[i].handle = 0;
            return &kasan_tlsf_instances[i].api;
        }
    }
    return 0;
}
