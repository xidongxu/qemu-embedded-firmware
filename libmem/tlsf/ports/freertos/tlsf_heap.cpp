/**
 * @file  tlsf_heap.cpp
 * @brief C++ global operator new/delete overrides routed to TLSF.
 *
 * Every new/new[]/delete/delete[] (plain, array, sized and nothrow forms)
 * goes through the unified TLSF allocator so C++ allocations share the same
 * pool as C malloc and the FreeRTOS kernel heap.
 *
 * The project is built with -fno-exceptions -fno-rtti; nothrow forms are
 * still provided for completeness.
 */
#include <cstddef>
#include <new>

/* TLSF integration layer (C linkage). */
extern "C" void *tlsf_port_malloc(size_t n);
extern "C" void tlsf_port_free(void *p);

void *operator new(std::size_t sz) {
    return tlsf_port_malloc(sz);
}

void *operator new[](std::size_t sz) {
    return tlsf_port_malloc(sz);
}

void *operator new(std::size_t sz, const std::nothrow_t &) noexcept {
    return tlsf_port_malloc(sz);
}

void *operator new[](std::size_t sz, const std::nothrow_t &) noexcept {
    return tlsf_port_malloc(sz);
}

void operator delete(void *p) noexcept {
    tlsf_port_free(p);
}

void operator delete[](void *p) noexcept {
    tlsf_port_free(p);
}

void operator delete(void *p, std::size_t) noexcept {
    tlsf_port_free(p);
}

void operator delete[](void *p, std::size_t) noexcept {
    tlsf_port_free(p);
}

void operator delete(void *p, const std::nothrow_t &) noexcept {
    tlsf_port_free(p);
}

void operator delete[](void *p, const std::nothrow_t &) noexcept {
    tlsf_port_free(p);
}
