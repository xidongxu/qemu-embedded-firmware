# littlefs - ARM-optimised embedded filesystem (upstream, v2.11)
#
# Upstream littlefs ships no CMake build, so we provide one here.  Core
# sources live at the repository root (lfs.c / lfs_util.c).  The block
# device backend is the on-board SPI NOR flash driver
# (boards/<BOARD>/Core/Src/spi_flash.c) glued in ports/spi_flash/.
#
# The SPI flash driver itself is compiled into the application target
# (boards/<BOARD>/<PROJECT> globs ../Core/Src/*.c), so the spi_flash_*
# symbols referenced by the port resolve at final link - no
# target_link_libraries() needed here.
#
# Optional multi-task safety: define LFS_THREADSAFE to enable the
# lock/unlock callbacks (mapped to spi_flash_os_lock()/spi_flash_os_unlock()):
#   target_compile_definitions(littlefs PUBLIC LFS_THREADSAFE=1)

add_library(littlefs STATIC
    lfs.c
    lfs_util.c
    ports/spi_flash/lfs_port_spi_flash.c
)

target_include_directories(littlefs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ports/spi_flash
    ${CMAKE_SOURCE_DIR}/boards/${BOARD}/Core/Inc
)
