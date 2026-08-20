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
  ${PJPROJECT_ROOT}/pjlib/src/pj/sock_qos_dummy.c
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
  src/os_rwmutex_freertos.c
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

# ##############################################################################
# pjlib-util (DNS resolver, scanner, xml, crypto used by pjsip)
# ##############################################################################
add_library(pjlib-util STATIC
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/base64.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/crc32.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/dns.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/errno.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/hmac_md5.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/hmac_sha1.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/md5.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/resolver.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/scanner.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/sha1.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/srv_resolver.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/string.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/xml.c
)
target_include_directories(pjlib-util PUBLIC ${PJPROJECT_ROOT}/pjlib-util/include)
target_link_libraries(pjlib-util PUBLIC pjlib)

# ##############################################################################
# pjsip core (no TLS transport)
# ##############################################################################
add_library(pjsip STATIC
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_auth_aka.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_auth_client.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_auth_msg.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_auth_parser.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_auth_server.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_config.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_dialog.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_endpoint.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_errno.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_msg.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_multipart.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_parser.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_resolve.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_tel_uri.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_transaction.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_transport.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_transport_loop.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_transport_tcp.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_transport_udp.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_ua_layer.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_uri.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_util.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_util_proxy.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip/sip_util_statefull.c
)
target_include_directories(pjsip PUBLIC ${PJPROJECT_ROOT}/pjsip/include)
target_link_libraries(pjsip PUBLIC pjlib pjlib-util)

# ##############################################################################
# pjsip-simple (SIMPLE/event subscription)
# ##############################################################################
add_library(pjsip-simple STATIC
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/errno.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/evsub.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/evsub_msg.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/iscomposing.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/mwi.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/pidf.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/dialog_info.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/presence.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/dlg_event.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/presence_body.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/publishc.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/rpid.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-simple/xpidf.c
)
target_include_directories(pjsip-simple PUBLIC ${PJPROJECT_ROOT}/pjsip/include)
target_link_libraries(pjsip-simple PUBLIC pjlib pjsip PRIVATE pjlib-util)

# ##############################################################################
# pjmedia (full framework trial: SDP + RTP + RTCP + event + codec mgr...)
# ##############################################################################
add_library(pjmedia STATIC
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/sdp.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/sdp_neg.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/sdp_cmp.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/types.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/rtp.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/jbuf.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/alaw_ulaw_table.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/errno.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/format.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/event.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/rtcp.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/rtcp_fb.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/rtcp_xr.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/stream_info.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/endpoint.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/codec.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/g711.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/stream_common.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/port.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/master_port.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/null_port.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/session.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/stream.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/av_sync.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/transport_udp.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/transport_loop.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/silencedet.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/tonegen.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/clock_thread.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/conference.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/conf_switch.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/splitcomb.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/bidirectional.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/mem_capture.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/mem_player.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/delaybuf.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/echo_common.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/echo_port.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/echo_suppress.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/plc_common.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/resample_port.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/resample_resample.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/wav_player.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/wav_writer.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/wave.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/stereo_port.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/wsola.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/audiodev.c
  src/audiodev_stub.c
  src/codec_stub.c
)
target_include_directories(pjmedia PUBLIC ${PJPROJECT_ROOT}/pjmedia/include)
target_link_libraries(pjmedia PUBLIC pjlib PRIVATE pjlib-util)

# ##############################################################################
# pjsip-ua (INVITE session + REGISTER client; needs pjmedia/SDP)
# ##############################################################################
add_library(pjsip-ua STATIC
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-ua/sip_inv.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-ua/sip_reg.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-ua/sip_replaces.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-ua/sip_xfer.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-ua/sip_100rel.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-ua/sip_timer.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsip-ua/sip_siprec.c
)
target_include_directories(pjsip-ua PUBLIC ${PJPROJECT_ROOT}/pjsip/include)
target_link_libraries(pjsip-ua PUBLIC pjlib pjsip pjsip-simple pjmedia PRIVATE pjlib-util)
