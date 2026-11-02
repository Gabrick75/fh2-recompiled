// fh2 - the simulation's frame time ("tempo" means "time").
//
// HOW THE GAME MEASURES A STEP
//
// The main loop (sub_823FDB68), right after it wakes for a step (0x823FDD48, see fh2_passos.cpp), reads a
// millisecond clock through a virtual call (singleton 0x833F2D78, vtable 0x8224417C, slot 0 = sub_82E77380, which
// returns the kernel's millisecond tick from sub_82401A60 plus an offset) and subtracts the previous reading:
//
//   ms = now - prev;                      // cmplwi r30,4000 / li r30,16: more than 4 s counts as 16 ms
//   dt = ms * 0.001f;                     // f26 = 0.001
//   if (ms == 16 || ms == 17) dt = 1/59.94 (with an error accumulator)   // f28 = 0.01668335
//   dt = max(dt, 0.0001f);                // f29
//   game->dt (+448) = dt;                 // and f1 = dt into the world, car, camera and effects updates
//
// So a step simulates exactly the wall time since the last one, with no cap below 4 s. On the Xbox 360 the game
// is held at 30 fps and dt is always about 33 ms. Here the frame rate floats (24-26 fps on phones, the same on
// ShiftGlue, see docs/alteracoes_fh2.md 2026-10-13) and some frames take far longer: a shader or pipeline built on
// first use, a texture upload, the step gate holding a step for up to fh2_ritmo_passos_espera_max. A collision
// resolved over one of those steps pushes the cars apart with the whole step's worth of correction, and they fly
// or jump sideways.
//
// WHAT THIS FILE DOES
//
// Wraps sub_82E77380 at the two call sites of that loop (by return address) and:
//   - logs "[tempo]" every 10 s: the steps by wall time (<=34, 35-50, 51-100, 101-250, >250 ms), the longest, and
//     what the cap removed; and one "[tempo] passo longo" line per step over 100 ms, to match against the moment a
//     car flew;
//   - with fh2_dt_max_ms > 0, makes the clock the loop sees advance at most that much per step. The time cut off
//     is kept as an offset, so the game runs slower during a hitch instead of taking it in one step (as the
//     console would: it never had steps that long). Turning it off keeps the offset, so dt never jumps.

#include <algorithm>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_INT32(fh2_dt_max_ms, 0, "FH2",
                     "Longest step the simulation is given, in ms (0: the wall time, as the game does). Above it the "
                     "game slows down for that step instead of simulating the whole hitch at once, which throws cars "
                     "apart in collisions. 34 = the console's 30 fps step; 50 cuts only the hitches "
                     "(app/src/fh2_tempo.cpp)")
    .range(0, 250)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(fh2_medir_tempo, true, "FH2",
                    "Log the simulation's step times every 10 s ([tempo]) and every step over 100 ms "
                    "(app/src/fh2_tempo.cpp)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_82E77380);

namespace {

// Return addresses of the clock reads in sub_823FDB68: before the loop, and once per step.
constexpr uint32_t kLoopStartRead = 0x823FDC10;
constexpr uint32_t kStepRead = 0x823FDDB4;
// The game's own guard: more than this counts as 16 ms, so the cap leaves it alone.
constexpr uint32_t kGameDiscardMs = 4000;
constexpr uint32_t kLongStepMs = 100;
constexpr uint32_t kReportMs = 10000;
constexpr uint32_t kLongLinesPerReport = 20;

// Only the main loop's thread reaches these two call sites.
uint32_t g_offset = 0;     // ms the cap has removed so far (game clock = real clock - offset)
uint32_t g_prev_real = 0;  // real clock at the previous read
bool g_have_prev = false;

struct Report {
  uint32_t start = 0;
  uint32_t steps = 0;
  uint32_t buckets[5] = {};  // <=34, 35-50, 51-100, 101-250, >250
  uint32_t longest = 0;
  uint32_t capped = 0;
  uint64_t removed = 0;
  uint32_t long_lines = 0;
} g_report;

uint32_t Bucket(uint32_t ms) {
  if (ms <= 34) return 0;
  if (ms <= 50) return 1;
  if (ms <= 100) return 2;
  if (ms <= 250) return 3;
  return 4;
}

void Note(uint32_t real_now, uint32_t ms, uint32_t given) {
  if (!REXCVAR_GET(fh2_medir_tempo)) {
    return;
  }
  Report& r = g_report;
  if (r.steps == 0 && r.start == 0) {
    r.start = real_now;
  }
  ++r.steps;
  ++r.buckets[Bucket(ms)];
  r.longest = std::max(r.longest, ms);
  if (given < ms) {
    ++r.capped;
    r.removed += ms - given;
  }
  if (ms > kLongStepMs && r.long_lines < kLongLinesPerReport) {
    ++r.long_lines;
    REXLOG_INFO("[tempo] passo longo: {} ms, simulado como {} ms", ms, given);
  }
  if (real_now - r.start >= kReportMs) {
    REXLOG_INFO(
        "[tempo] passos={} <=34={} 35-50={} 51-100={} 101-250={} >250={} maior={} ms | dt_max={} limitados={} "
        "cortados={} ms atraso_total={} ms",
        r.steps, r.buckets[0], r.buckets[1], r.buckets[2], r.buckets[3], r.buckets[4], r.longest,
        REXCVAR_GET(fh2_dt_max_ms), r.capped, r.removed, g_offset);
    r = Report{};
    r.start = real_now;
  }
}

}  // namespace

REX_HOOK_RAW(sub_82E77380) {
  const uint32_t site = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82E77380(ctx, base);
  if (site != kStepRead && site != kLoopStartRead) {
    return;
  }
  const uint32_t real_now = ctx.r3.u32;
  if (site == kStepRead && g_have_prev) {
    const uint32_t ms = real_now - g_prev_real;
    uint32_t given = ms;
    const int32_t cap = REXCVAR_GET(fh2_dt_max_ms);
    if (cap > 0 && ms > uint32_t(cap) && ms <= kGameDiscardMs) {
      given = uint32_t(cap);
      g_offset += ms - given;
    }
    Note(real_now, ms, given);
  }
  g_prev_real = real_now;
  g_have_prev = true;
  ctx.r3.u64 = uint32_t(real_now - g_offset);
}
