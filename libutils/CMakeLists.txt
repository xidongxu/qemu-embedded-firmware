add_subdirectory(fault-dump)
if(TARGET_PROJECT STREQUAL "FreeRTOS")
    set(LV_BUILD_CONF_PATH ${CMAKE_SOURCE_DIR}/boards/${BOARD}/${PROJECT}/application/lv_conf.h CACHE FILEPATH "" FORCE)
    set(LVGL_ROOT_DIR ${CMAKE_SOURCE_DIR}/libutils/lvgl/ CACHE FILEPATH "" FORCE)
    add_subdirectory(lvgl)
endif()
if(TARGET_PROJECT STREQUAL "FreeRTOS")
    set(LWIP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lwip)
    set(LWIP_INCLUDE_DIRS
        ${LWIP_DIR}/src/include
        ${LWIP_DIR}/ports
    )
    set(LWIP_COMPILER_FLAGS "")
    set(LWIP_DEFINITIONS "")
    set(LWIP_MBEDTLS_DEFINITIONS "")
    set(LWIP_MBEDTLS_INCLUDE_DIRS "")
    include(${LWIP_DIR}/src/Filelists.cmake)
endif()
