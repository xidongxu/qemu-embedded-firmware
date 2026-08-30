# TLSF (Two-Level Segregated Fit) memory allocator.
#
# tlsf.c is the pure allocator core (RTOS-independent).
#
# For the FreeRTOS project an integration layer is also compiled:
#   - ports/freertos/tlsf_port.c    unified pool + C malloc hooks (--wrap)
#   - ports/freertos/tlsf_heap.cpp  C++ operator new/delete overrides
#
# The FreeRTOS kernel heap (pvPortMalloc/pvPortFree) is switched to this
# allocator by setting FREERTOS_HEAP to ports/freertos/tlsf_freertos.c in the
# top-level CMakeLists.
add_library(tlsf STATIC tlsf.c)

target_include_directories(tlsf PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ports/freertos
)

# Integration layer compiled only for the FreeRTOS project with the unified
# TLSF allocator enabled (TLSF_MALLOC).
if(TARGET_PROJECT STREQUAL "FreeRTOS" AND TLSF_MALLOC)
    target_sources(tlsf PRIVATE
        ports/freertos/tlsf_port.c
        ports/freertos/tlsf_heap.cpp
    )
    # Pull the FreeRTOS public headers (FreeRTOS.h, portmacro.h, task.h)
    # for the critical-section guards in tlsf_port.c.
    target_link_libraries(tlsf PRIVATE freertos_kernel)
endif()
