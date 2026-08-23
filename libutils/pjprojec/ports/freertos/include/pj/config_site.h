/*
 * config_site.h
 *
 * PJPROJECT site configuration for the FreeRTOS / Cortex-M port.
 * This file is included by pj/config.h. Put this directory first in the
 * include path so this copy shadows any other config_site.h.
 */
#ifndef PJ_CONFIG_SITE_H
#define PJ_CONFIG_SITE_H

/* ---- PJLIB core ------------------------------------------------------ */

/* Cortex-M has no FPU in some configurations and float printf pulls in a
 * lot of code; disable floating point support for the embedded port. */
#define PJ_HAS_FLOATING_POINT           0

/* Keep debug output reasonable on a serial console. */
#define PJ_LOG_MAX_LEVEL                4

/* Embedded: keep the per-log stack buffer small (4000-byte default would
 * eat into the 8 KB FreeRTOS task stack). */
#define PJ_LOG_MAX_SIZE                 512

#define PJ_DEBUG                        0

#define PJ_MAX_OBJ_NAME                 32

/* Default thread stack: 16 KiB (4 K words) - pjsua 200-OK/media handling
 * needs a deep call chain; 8 KiB still overflowed (2026-08-2x).
 * Overridable at runtime. */
#define PJ_THREAD_DEFAULT_STACK_SIZE    16384

/* Default thread priority (FreeRTOS, higher = more important). */
#define PJ_FREERTOS_DEFAULT_PRIO        2

/* We implement thread stacks ourselves (FreeRTOS task stack). */
#define PJ_THREAD_ALLOCATE_STACK        0

/* Atomic ops are emulated with a mutex in os_core_freertos.c. */
#define PJ_ATOMIC_VALUE_TYPE            long

/* Pool alignment (must be power of two). */
#ifndef PJ_POOL_ALIGNMENT
#   define PJ_POOL_ALIGNMENT            8
#endif

/* PJLIB is initialized from a FreeRTOS task, no libc startup work needed. */
#define PJ_HAS_MALLOC                   1

/* Stack checking is not available on FreeRTOS tasks. */
#define PJ_OS_HAS_CHECK_STACK           0

/* No OS-specific QoS backend for this target: use the dummy one. */
#define PJ_QOS_IMPLEMENTATION           PJ_QOS_DUMMY

/* Suppress the (intentional) unused-label warning from PJ_TODO(). */
#define PJ_TODO(x)

/* ---- PJSIP (stage 3) ------------------------------------------------ */

/* Small embedded sizing: keep the transaction/dialog tables tiny. */
#ifndef PJSIP_MAX_TSX_COUNT
#   define PJSIP_MAX_TSX_COUNT          16
#endif
#ifndef PJSIP_MAX_DIALOG_COUNT
#   define PJSIP_MAX_DIALOG_COUNT       8
#endif
#ifndef PJSIP_MAX_TRANSPORTS
#   define PJSIP_MAX_TRANSPORTS         4
#endif
#define PJSIP_HAS_TLS_TRANSPORT         0

/* ---- PJMEDIA (full-framework trial, stage 14) ----------------------- */
/* No external media backends / codecs on this embedded target; keep only
 * the built-in G.711 (PCMU/PCMA) codec.  pjmedia/config.h uses #ifndef so
 * defining these here (config_site.h is included first) overrides defaults. */
#define PJMEDIA_HAS_VIDEO               0
#define PJMEDIA_HAS_SRTP                0
#define PJMEDIA_HAS_FFMPEG              0
#define PJMEDIA_HAS_LIBYUV              0
#define PJMEDIA_HAS_SPEEX_AEC           0
#define PJMEDIA_HAS_WEBRTC_AEC          0
#define PJMEDIA_HAS_WEBRTC_AEC3         0
#define PJMEDIA_RESAMPLE_IMP            PJMEDIA_RESAMPLE_NONE
#define PJMEDIA_HAS_G711_CODEC          1
#define PJMEDIA_HAS_G722_CODEC          0
#define PJMEDIA_HAS_G7221_CODEC         0
#define PJMEDIA_HAS_L16_CODEC           0
#define PJMEDIA_HAS_ILBC_CODEC          0
#define PJMEDIA_HAS_GSM_CODEC           0
#define PJMEDIA_HAS_SPEEX_CODEC         0
#define PJMEDIA_HAS_OPUS_CODEC          0
#define PJMEDIA_HAS_OPENCORE_AMRNB_CODEC 0
#define PJMEDIA_HAS_OPENCORE_AMRWB_CODEC 0
#define PJMEDIA_HAS_SILK_CODEC          0
#define PJMEDIA_HAS_VPX_CODEC           0
#define PJMEDIA_HAS_OPENH264_CODEC      0
#define PJMEDIA_HAS_BCG729              0
#define PJMEDIA_HAS_LYRA_CODEC          0
#define PJMEDIA_HAS_AUDIODEV            1
#define PJMEDIA_HAS_VIDEODEV            0

/* pjmedia-audiodev backend selection: only the null device for the
 * embedded target.  The defaults pick WMME (Windows) unless overridden. */
#define PJMEDIA_AUDIO_DEV_HAS_PORTAUDIO     0
#define PJMEDIA_AUDIO_DEV_HAS_OPENSL        0
#define PJMEDIA_AUDIO_DEV_HAS_ANDROID_JNI   0
#define PJMEDIA_AUDIO_DEV_HAS_OBOE          0
#define PJMEDIA_AUDIO_DEV_HAS_BB10          0
#define PJMEDIA_AUDIO_DEV_HAS_ALSA          0
#define PJMEDIA_AUDIO_DEV_HAS_COREAUDIO     0
#define PJMEDIA_AUDIO_DEV_HAS_WMME          0
#define PJMEDIA_AUDIO_DEV_HAS_WASAPI        0
#define PJMEDIA_AUDIO_DEV_HAS_BDIMAD        0
#define PJMEDIA_AUDIO_DEV_HAS_SYMB_APS      0
#define PJMEDIA_AUDIO_DEV_HAS_SYMB_VAS      0
#define PJMEDIA_AUDIO_DEV_HAS_SYMB_MDA      0
#define PJMEDIA_AUDIO_DEV_HAS_LEGACY_DEVICE 0
#define PJMEDIA_AUDIO_DEV_HAS_NULL_AUDIO    1
#define PJMEDIA_HAS_LEGACY_SOUND_API    0

/* Fixed hostfwd/slirp topology: the RTP source address seen by the stream
 * (slirp's forwarding port) differs from the SDP-negotiated address, and the
 * source port can vary per packet.  With probation count 0 the stream never
 * drops RTP based on source-address changes (no NAT detection needed).
 * A/B test (2026-08-20): CNT=10 vs CNT=0 show nearly identical empty/missing
 * stats (not the empty-frame cause; jbuf min_pre=0/min_delay=1 is), but CNT=0
 * removes the theoretical risk of dropping opening frames when slirp's
 * forwarding source port differs from the SDP address.  Keep as a sane
 * configuration for this fixed hostfwd topology. */
#define PJMEDIA_RTP_NAT_PROBATION_CNT   0

#endif  /* PJ_CONFIG_SITE_H */
