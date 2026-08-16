#
# CMakeLists.txt
#
# PJLIB FreeRTOS port library target.
#
# Stage 1: pjlib core only (threads/mutex/sem/timer/pool/log) + FreeRTOS OS
# port. Socket / ioqueue / SSL sources are intentionally NOT compiled yet.
#
# The port headers (os_auto.h, m_auto.h, config_site.h) live in
# ports/freertos/include and shadow the autoconf-generated ones because this
# directory is put FIRST in the include path.  Compile with -DPJ_AUTOCONF=1
# so pj/config.h picks up our hand-written os_auto.h / m_auto.h.

set(PJPROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

add_library(pjlib STATIC
  # ---- pjlib core ----
  ${PJPROJECT_ROOT}/pjlib/src/pj/array.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/atomic_slist.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/config.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/ctype.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/errno.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/except.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/fifobuf.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/guid.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/guid_simple.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/hash.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/list.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/lock.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/log.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/log_writer_stdout.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/os_info.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/os_time_common.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/os_timestamp_common.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/os_error_unix.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/pool.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/pool_buf.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/pool_caching.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/pool_dbg.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/pool_policy_malloc.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/rand.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/rbtree.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/string.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/timer.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/types.c
  # ---- socket / ioqueue layer (lwIP-backed) ----
  ${PJPROJECT_ROOT}/pjlib/src/pj/sock_common.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/sock_qos_common.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/activesock.c
  ${PJPROJECT_ROOT}/pjlib/src/pj/addr_resolv_sock.c
  src/sock_bsd.c
  src/sock_select_lwip.c
  src/ioqueue_select.c
  src/ip_helper_freertos.c
  # ---- FreeRTOS platform port ----
  src/os_core_freertos.c
  src/os_time_freertos.c
  src/os_timestamp_freertos.c
)

# First in the include path so our os_auto.h / m_auto.h / config_site.h
# shadow the autoconf-generated ones in pjlib/include.  The lwIP include
# dirs let pj/compat/socket.h (and friends) resolve to lwip/sockets.h.
target_include_directories(pjlib
  PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${PJPROJECT_ROOT}/pjlib/include
    ${PJPROJECT_ROOT}/pjlib/src/pj
    ${CMAKE_SOURCE_DIR}/libutils/lwip/src/include
    ${CMAKE_SOURCE_DIR}/libutils/lwip/ports
)

# pj/config.h selects our hand-written os_auto.h/m_auto.h via PJ_AUTOCONF.
# PUBLIC so that consumers of pjlib headers (e.g. the self-test app) get it too.
# LWIP_USE_FREERTOS makes lwipopts.h select OS mode (NO_SYS=0 -> LWIP_SOCKET=1).
target_compile_definitions(pjlib
  PUBLIC
    PJ_AUTOCONF=1
  PRIVATE
    LWIP_USE_FREERTOS=1
)

# FreeRTOS kernel headers + FreeRTOSConfig.h (via freertos_kernel_include and
# freertos_config), exactly like the lwIP FreeRTOS port does.
target_link_libraries(pjlib
  PRIVATE
    freertos_kernel
    lwipcore
)
