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

# ---------------------------------------------------------------------------
# Generate the autoconf-style headers that upstream pjproject's own CMake
# produces via configure_file() in pjlib/CMakeLists.txt, pjsip/CMakeLists.txt,
# pjmedia/CMakeLists.txt, ...  Those upstream files are NOT part of this port
# (each module is built by hand below), so without this step the generated
# headers would be missing and every compile of pjsip / pjmedia fails with
# "No such file or directory" (sip_autoconf.h / config_auto.h).  We reproduce
# the generation here, writing into ports/freertos/include which is already on
# every target's include path - same "shadow the autoconf header" pattern the
# hand-written os_auto.h / m_auto.h / config_site.h use.  Output files are
# gitignored (see libutils/pjprojec/.gitignore).
# ---------------------------------------------------------------------------
# PJMEDIA: only the built-in G.711 codec is enabled (see pj/config_site.h).
set(PJMEDIA_HAS_G711_CODEC 1)
# PJSIP: no TLS transport on this embedded target (config_site.h sets 0 too).
set(PJSIP_HAS_TLS_TRANSPORT 0)

configure_file(
  ${PJPROJECT_ROOT}/pjsip/include/pjsip/sip_autoconf.h.cm
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pjsip/sip_autoconf.h
)
configure_file(
  ${PJPROJECT_ROOT}/pjmedia/include/pjmedia/config_auto.h.cm
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pjmedia/config_auto.h
)
configure_file(
  ${PJPROJECT_ROOT}/pjmedia/include/pjmedia-codec/config_auto.h.cm
  ${CMAKE_CURRENT_SOURCE_DIR}/include/pjmedia-codec/config_auto.h
)

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
  ${PJPROJECT_ROOT}/pjlib/src/pj/file_io_ansi.c
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
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/stun_simple_client.c
  ${PJPROJECT_ROOT}/pjlib-util/src/pjlib-util/stun_simple.c
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
# bundled Speex (AEC backend).  Defined HERE instead of add_subdirectory() on
# third_party/speex so the upstream pjproject tree stays unmodified: the
# upstream CMakeLists does `find_library(MATH_LIBRARY m REQUIRED)` which fails
# on bare-metal (no libm file; newlib provides the math symbols).  We only
# need the AEC subset; the stereo decorrelator (scal.c) is compiled only if
# referenced, and our echo_speex.c overlay compiles it out (see
# src/pjmedia/echo_speex.c + PJMEDIA_SPEEX_AEC_NO_DECORRELATE in config_site.h).
add_library(speex STATIC
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/bits.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/cb_search.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/exc_10_16_table.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/exc_10_32_table.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/exc_20_32_table.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/exc_5_256_table.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/exc_5_64_table.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/exc_8_128_table.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/fftwrap.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/filterbank.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/filters.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/gain_table.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/gain_table_lbr.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/hexc_10_32_table.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/hexc_table.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/high_lsp_tables.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/kiss_fft.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/kiss_fftr.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/lpc.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/lsp.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/lsp_tables_nb.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/ltp.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/mdf.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/modes.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/modes_wb.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/nb_celp.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/preprocess.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/quant_lsp.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/resample.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/sb_celp.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/smallft.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/speex.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/speex_callbacks.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/speex_header.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/scal.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/stereo.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/vbr.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/vq.c
  ${PJPROJECT_ROOT}/third_party/speex/libspeex/window.c
)
# Public so consumers (pjmedia compiles the echo_speex.c overlay, which includes
# <speex/speex_echo.h> and transitively speex/speex_config_types.h) get them.
target_include_directories(speex PUBLIC
  ${PJPROJECT_ROOT}/third_party/speex/include
  ${PJPROJECT_ROOT}/third_party/build/speex
)
target_compile_definitions(speex PRIVATE HAVE_CONFIG_H=1)
# speex (mdf.c / preprocess.c) uses libm (sin/pow/sqrt).  Link it PUBLIC so any
# target using speex also links libm AFTER speex in the link line - CMake
# places a library's PUBLIC deps right after it, which matters for static lib
# symbol resolution (an -lm placed earlier never resolves speex's math refs).
target_link_libraries(speex PUBLIC m PRIVATE pjlib)

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
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/echo_speex.c
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
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/sound_port.c
  src/codec_stub.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/txt_stream.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia/transport_ice.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia-codec/audio_codecs.c
)
target_include_directories(pjmedia PUBLIC
  ${PJPROJECT_ROOT}/pjmedia/include
  ${PJPROJECT_ROOT}/pjnath/include
)
target_link_libraries(pjmedia PUBLIC pjlib speex PRIVATE pjlib-util)

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

# ##############################################################################
# pjnath (STUN/TURN/ICE - pjsua-lib references ice_strans unconditionally)
# ##############################################################################
add_library(pjnath STATIC
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/errno.c
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/ice_session.c
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/ice_strans.c
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/nat_detect.c
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/stun_auth.c
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/stun_msg.c
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/stun_msg_dump.c
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/stun_session.c
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/stun_sock.c
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/stun_transaction.c
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/turn_session.c
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/turn_sock.c
  ${PJPROJECT_ROOT}/pjnath/src/pjnath/upnp.c
)
target_include_directories(pjnath PUBLIC ${PJPROJECT_ROOT}/pjnath/include)
target_link_libraries(pjnath PUBLIC pjlib pjlib-util)

# ##############################################################################
# pjmedia-audiodev (sound-device abstraction; null backend for embedded)
# ##############################################################################
add_library(pjmedia-audiodev STATIC
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia-audiodev/audiodev.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia-audiodev/null_dev.c
  ${PJPROJECT_ROOT}/pjmedia/src/pjmedia-audiodev/errno.c
)
target_include_directories(pjmedia-audiodev PUBLIC ${PJPROJECT_ROOT}/pjmedia/include)
target_link_libraries(pjmedia-audiodev PUBLIC pjlib PRIVATE pjmedia pjlib-util)

# ##############################################################################
# pjsua-lib (high-level PJSUA API)
# ##############################################################################
add_library(pjsua-lib STATIC
  ${PJPROJECT_ROOT}/pjsip/src/pjsua-lib/pjsua_acc.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsua-lib/pjsua_aud.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsua-lib/pjsua_call.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsua-lib/pjsua_core.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsua-lib/pjsua_dump.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsua-lib/pjsua_im.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsua-lib/pjsua_media.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsua-lib/pjsua_pres.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsua-lib/pjsua_txt.c
  ${PJPROJECT_ROOT}/pjsip/src/pjsua-lib/pjsua_vid.c
)
target_include_directories(pjsua-lib PUBLIC
  ${PJPROJECT_ROOT}/pjsip/include
  ${PJPROJECT_ROOT}/pjnath/include
)
target_link_libraries(pjsua-lib PUBLIC
  pjlib pjlib-util pjnath pjmedia pjmedia-audiodev
  pjsip pjsip-simple pjsip-ua
)
