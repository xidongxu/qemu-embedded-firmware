# kasan host unit tests
#
# test_kasan.c #includes ../../kasan.c with a RAM-backed fake region so the
# allocator/shadow logic can run and be asserted on the host (no ARM, no
# QEMU, no -fsanitize needed).  KASAN_TEST_RETURNS makes kasan_report()
# return instead of trapping.
#
# The library models 32-bit addresses (uint32_t), so the host build MUST be
# 32-bit too (Linux: -m32 / gcc-multilib; Windows: a 32-bit target).
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "kasan host tests model 32-bit addresses; build 32-bit "
                    "(-m32 / 32-bit toolchain) or the pointer casts truncate")
endif()

add_executable(test_kasan test_kasan.c)
target_include_directories(test_kasan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..          # kasan.h (via kasan.c)
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../libmem/tlsf  # tlsf.h (via backend)
)
# The library models 32-bit addresses (uint32_t); on 64-bit Windows pin the
# image base below 4 GB so the pointer casts do not truncate (Linux: -m32).
if(WIN32)
    target_link_options(test_kasan PRIVATE "-Wl,--image-base=0x1000000")
endif()
add_test(NAME kasan_host COMMAND test_kasan)
