/*
 * opus.h - port forwarding header (pjmedia-codec/opus.c includes
 * <opus/opus.h>; libopus keeps its public headers flat in include/).
 *
 * The relative path keeps the inner #include "opus_defines.h" / "opus_types.h"
 * resolving to libutils/opus/include.
 */
#include "../../../include/opus.h"
