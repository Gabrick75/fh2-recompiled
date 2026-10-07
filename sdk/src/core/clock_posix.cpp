/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/chrono/clock.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <sys/time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#if REX_PLATFORM_SWITCH
// Only the counter header: switch.h drags typedefs (Thread, Mutex, Event) that
// collide with rex::thread.
#include <switch/types.h>
#include <switch/arm/counter.h>
#endif

namespace rex::chrono {

uint64_t Clock::host_tick_frequency_platform() {
#ifdef __APPLE__
  mach_timebase_info_data_t info;
  mach_timebase_info(&info);
  return (uint64_t)((1000000000ull * (uint64_t)info.denom) / (uint64_t)info.numer);
#elif REX_PLATFORM_SWITCH
  // libnx only accepts CLOCK_REALTIME and CLOCK_MONOTONIC: CLOCK_MONOTONIC_RAW
  // fails with EINVAL and the assert below aborted the process during static
  // initialization. CLOCK_MONOTONIC is not a drop-in either: clock_getres
  // reports 52 ns while clock_gettime returns nanoseconds, so the formula below
  // would make guest time run 52 times slower. The system counter is exact and
  // a single register read.
  return armGetSystemTickFreq();
#else
  timespec res;
  int error = clock_getres(CLOCK_MONOTONIC_RAW, &res);
  assert_zero(error);
  assert_zero(res.tv_sec);  // Sub second resolution is required.

  // Convert nano seconds to hertz. Resolution is 1ns on most systems.
  return 1000000000ull / res.tv_nsec;
#endif
}

uint64_t Clock::host_tick_count_platform() {
#ifdef __APPLE__
  // mach_absolute_time() returns raw hardware ticks; host_tick_frequency_platform()
  // expresses the rate of these same ticks via mach_timebase_info.
  // clock_gettime(CLOCK_MONOTONIC_RAW) returns nanoseconds, which would be
  // inconsistent with the 24 MHz mach frequency and cause ~41x time acceleration.
  return mach_absolute_time();
#elif REX_PLATFORM_SWITCH
  // Same ticks that host_tick_frequency_platform() measures.
  return armGetSystemTick();
#else
  timespec tp;
  int error = clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
  assert_zero(error);

  return tp.tv_nsec + tp.tv_sec * 1000000000ull;
#endif
}

uint64_t Clock::QueryHostSystemTime() {
  // https://docs.microsoft.com/en-us/windows/win32/sysinfo/converting-a-time-t-value-to-a-file-time
  constexpr uint64_t seconds_per_day = 3600 * 24;
  // Don't forget the 89 leap days.
  constexpr uint64_t seconds_1601_to_1970 = ((369 * 365 + 89) * seconds_per_day);

  timeval now;
  int error = gettimeofday(&now, nullptr);
  assert_zero(error);

  // NT systems use 100ns intervals.
  return static_cast<uint64_t>(
      (static_cast<int64_t>(now.tv_sec) + seconds_1601_to_1970) * 10000000ull + now.tv_usec * 10);
}

uint64_t Clock::QueryHostUptimeMillis() {
  return host_tick_count_platform() * 1000 / host_tick_frequency_platform();
}

}  // namespace rex::chrono
