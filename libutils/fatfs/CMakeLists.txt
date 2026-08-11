# FatFs - generic FAT filesystem module (ChaN, R0.16)
#
# Upstream FatFs ships no CMake build; we provide one here.  The core
# module (ff.c) plus the platform layer (ffsystem.c) and the Unicode tables
# (ffunicode.c) are compiled from source/.  The low-level disk backend for
# the SPI NOR flash lives in ports/spi_flash/.
#
# NOTE: the upstream template source/diskio.c is intentionally NOT compiled -
# the real disk_* implementation is ports/spi_flash/diskio_spi_flash.c, so
# compiling the template would create duplicate symbols.
#
# Configuration: R0.16 ff.h only includes ffconf.h when FFCONF_DEF is not
# already defined.  We force-include ports/spi_flash/ffconf.h (see
# target_compile_options below), so the project config lives entirely in the
# ports tree and the upstream source/ffconf.h stays pristine.  Tuned for the
# SPI flash backend: FF_USE_MKFS / FF_USE_LFN / FF_CODE_PAGE / FF_USE_TRIM.
#
# The SPI flash driver itself is compiled into the application target
# (boards/<BOARD>/<PROJECT> globs ../Core/Src/*.c), so the spi_flash_*
# symbols referenced by the port resolve at final link - no
# target_link_libraries() needed here.

add_library(fatfs STATIC
    source/ff.c
    source/ffsystem.c
    source/ffunicode.c
    ports/spi_flash/diskio_spi_flash.c
)

target_include_directories(fatfs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${CMAKE_CURRENT_SOURCE_DIR}/ports/spi_flash
    ${CMAKE_SOURCE_DIR}/boards/${BOARD}/Core/Inc
)

# Force the project ffconf.h ahead of any #include "ffconf.h" (ff.h skips the
# upstream one once FFCONF_DEF is defined).  PUBLIC so every TU that includes
# ff.h - including application code linking this library - sees the same
# config and thus the same FIL/FATFS struct layout (ABI consistency).
target_compile_options(fatfs PUBLIC
    "-include;${CMAKE_CURRENT_SOURCE_DIR}/ports/spi_flash/ffconf.h"
)
