/* FFmpeg configuration for Nintendo Switch (Horizon, aarch64, devkitA64).
 *
 * The Xenia FFmpeg fork ships one config header per desktop/mobile OS and none
 * for Horizon. Linux/aarch64 is the closest: same CPU features (NEON, ARMv8,
 * inline assembly) and pthreads, which libnx provides. Only what Horizon lacks
 * is turned off. Verified by compiling every libavutil and libavcodec source
 * the SDK builds (NEON .S files included) with devkitA64. */
#pragma once
#include "config_linux_aarch64.h"

#undef HAVE_SCHED_GETAFFINITY
#define HAVE_SCHED_GETAFFINITY 0
#undef HAVE_MMAP
#define HAVE_MMAP 0
#undef HAVE_SYSCONF
#define HAVE_SYSCONF 0
