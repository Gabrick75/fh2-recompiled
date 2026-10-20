// fh2 - who in the game sleeps or yields, and for how long ("esperas" means "waits").
//
//   --fh2_medir_esperas=true
//
// The game's XAPI Sleep (sub_82BF0A08, r3 = milliseconds), SleepEx (sub_82BF09D0) and SwitchToThread (sub_82BF0838)
// are counted per call site (return address) and argument. Every fh2_medir_esperas_s seconds the "[esperas]" lines
// give, for each site, the calls and the milliseconds spent per second, and the host thread that made them. Sleeps in
// the frame loop that only gave the Xbox 360's cores to another thread (as Black's frame throttle did, see
// ../referencias/Black-Android/src/game/hooks.cpp) show up here as a steady cost per second.

#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(fh2_medir_esperas, false, "FH2",
                    "Debug: count the game's Sleep and SwitchToThread calls per call site (app/src/fh2_esperas.cpp)");
REXCVAR_DEFINE_DOUBLE(fh2_medir_esperas_s, 10.0, "FH2", "Debug: seconds between the [esperas] reports");

REX_EXTERN(__imp__sub_82BF0A08);
REX_EXTERN(__imp__sub_82BF09D0);
REX_EXTERN(__imp__sub_82BF0838);

namespace {

using Clock = std::chrono::steady_clock;

enum Kind : uint32_t { kSleep, kSleepEx, kYield };
constexpr const char* kKindNames[] = {"Sleep", "SleepEx", "SwitchToThread"};

struct Site {
  uint64_t calls = 0;
  uint64_t ns = 0;
  long tid = 0;  // the last host thread that called from here
};

std::mutex g_lock;
std::unordered_map<uint64_t, Site> g_sites;  // kind << 62 | argument << 32 | return address
Clock::time_point g_since;

std::string ThreadName(long tid) {
  std::ifstream comm("/proc/self/task/" + std::to_string(tid) + "/comm");
  std::string name;
  std::getline(comm, name);
  return name.empty() ? "?" : name;
}

// Called with g_lock held.
void ReportIfDue(Clock::time_point now) {
  const double seconds = std::chrono::duration<double>(now - g_since).count();
  if (seconds < REXCVAR_GET(fh2_medir_esperas_s)) {
    return;
  }
  std::vector<std::pair<uint64_t, Site>> sites(g_sites.begin(), g_sites.end());
  std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
  REXLOG_INFO("[esperas] {:.1f} s, {} call sites", seconds, sites.size());
  for (const auto& [key, site] : sites) {
    REXLOG_INFO("[esperas] {:>14} ret {:08X} arg {:>5}: {:8.1f} calls/s {:7.2f} ms/s  thread \"{}\"",
                kKindNames[key >> 62], uint32_t(key), int32_t(uint32_t(key >> 32) & 0x3FFFFFFF),
                site.calls / seconds, site.ns / seconds / 1e6, ThreadName(site.tid));
  }
  g_sites.clear();
  g_since = now;
}

template <typename Call>
void Measure(Kind kind, PPCContext& ctx, Call call) {
  if (!REXCVAR_GET(fh2_medir_esperas)) {
    call();
    return;
  }
  const uint32_t return_address = static_cast<uint32_t>(ctx.lr);
  const uint32_t argument = kind == kYield ? 0 : (ctx.r3.u32 & 0x3FFFFFFF);
  const Clock::time_point start = Clock::now();
  call();
  const Clock::time_point end = Clock::now();
  std::lock_guard lock(g_lock);
  if (g_since == Clock::time_point()) {
    g_since = start;
  }
  Site& site = g_sites[uint64_t(kind) << 62 | uint64_t(argument) << 32 | return_address];
  site.calls++;
  site.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
  site.tid = static_cast<long>(syscall(SYS_gettid));
  ReportIfDue(end);
}

}  // namespace

REX_HOOK_RAW(sub_82BF0A08) { Measure(kSleep, ctx, [&] { __imp__sub_82BF0A08(ctx, base); }); }
REX_HOOK_RAW(sub_82BF09D0) { Measure(kSleepEx, ctx, [&] { __imp__sub_82BF09D0(ctx, base); }); }
REX_HOOK_RAW(sub_82BF0838) { Measure(kYield, ctx, [&] { __imp__sub_82BF0838(ctx, base); }); }
