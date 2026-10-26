// fh2 - one simulation step per frame shown ("passos" means "steps").
//
// THE STEP EVENT AND THE VBLANK
//
// The game's main loop (sub_823FDB68) runs one simulation step each time it wakes from an event (handle at
// 0x834241E0, the wait returns to 0x823FDD48). The game's vblank callback (sub_825F5530, registered with the D3D
// through sub_82B71C38) tail calls sub_8248BA70, which counts vblanks at 0x834241EC and sets that event every
// second vblank (every vblank in one mode), whatever the main and render threads are doing; the render thread
// then draws the newest finished step. So when a frame takes longer than two vblanks, steps are skipped, and when
// the two drift apart a frame gets none: at a flat frame rate frames show 0 or 2 steps of motion, and the car
// judders. The native renderer measured 2.45 vblanks per swap on the S23 (nfsmw_ajustes_graficos.cpp), right in
// that case.
//
// The gate: while the main loop is two steps ahead of the frames the render thread has swapped, the vblank does
// not set the event and keeps its count (so every later vblank asks again); the render thread's next VdSwap sets
// it instead. Steps that keep up stay on the vblank. After fh2_ritmo_passos_espera_max ms without a swap (loading,
// a stalled render thread) the step runs anyway.
//
// Ported from Pinyon Shift (Forza Horizon 1, same engine), issue #381 and cvar pinyon_shift_step_backpressure,
// where frames with exactly one step went from 86-87 % to about 99 % in a race (src/pinyon_shift_runtime_hooks.cpp,
// PinyonShiftGateSimulationStep and PinyonShiftNoteTitleSwap). FH1 hooks the decision mid-function; here the
// whole of sub_8248BA70 is replaced, it is short.
//
// The log line "[passos]" every 10 s gives the frames with 0/1/2/3+ steps and what the gate did, with the gate on
// or off (fh2_ritmo_passos), for an A/B.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>

REXCVAR_DEFINE_BOOL(fh2_ritmo_passos, true, "FH2",
                    "Start a simulation step only when the render thread has swapped all but the latest one, so "
                    "every frame shown advances the game by one step instead of 0 or 2 (app/src/fh2_passos.cpp)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(fh2_ritmo_passos_espera_max, 50.0, "FH2",
                      "With fh2_ritmo_passos, the longest a step waits for the render thread before it runs anyway "
                      "(ms)")
    .range(5.0, 250.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_8248BA70);
REX_EXTERN(__imp__sub_82475730);
REX_EXTERN(sub_824893E8);
REX_EXTERN(sub_8248A468);

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kStepEventHandle = 0x834241E0;
constexpr uint32_t kStepVblankCount = 0x834241EC;
// Byte that, with the settings' +2402 flag, keeps the one-step-every-vblank mode (lbz 16873 in sub_8248BA70).
constexpr uint32_t kStepEveryVblankOff = 0x834241E9;
// The settings object (sub_824893E8): +2403 every second vblank, +2402 every second vblank unless 0x834241E9.
constexpr uint32_t kSettingsEverySecond = 2403;
constexpr uint32_t kSettingsEverySecondUnless = 2402;
// Return address of the main loop's wait for the step event (sub_823FDB68).
constexpr uint32_t kMainLoopWaitReturn = 0x823FDD48;

uint32_t LoadGuestU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  __builtin_memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

void StoreGuestU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  __builtin_memcpy(base + address, &value, sizeof(value));
}

// Steps the main loop has started and frames the render thread has swapped.
std::atomic<uint64_t> g_steps{0};
std::atomic<uint64_t> g_swaps{0};
// A raise of the step event is held (the vblank sets it, the swap or a later vblank clears it).
std::atomic<bool> g_held{false};
// Steps minus swaps when they last lined up (vblank and swap threads, under g_gate_mutex).
std::mutex g_gate_mutex;
int64_t g_gate_base = 0;
// Guest memory, for the swap's raise (seen by the vblank hook first).
std::atomic<uint8_t*> g_base{nullptr};

// What the gate did, for the log (vblank thread writes, swap thread reads and clears).
std::atomic<uint32_t> g_raises{0}, g_holds{0}, g_swap_raises{0}, g_timeouts{0};
std::atomic<uint64_t> g_held_us{0};

int64_t StepLead() {
  return int64_t(g_steps.load(std::memory_order_acquire)) - int64_t(g_swaps.load(std::memory_order_acquire)) -
         g_gate_base;
}

// The step event raised as sub_8248BA70 does: restart the vblank count and set the event.
void RaiseStepEvent() {
  uint8_t* base = g_base.load(std::memory_order_acquire);
  auto* kernel = rex::system::kernel_state();
  if (!base || !kernel) {
    return;
  }
  StoreGuestU32(base, kStepVblankCount, 0);
  if (auto event = kernel->object_table()->LookupObject<rex::system::XEvent>(LoadGuestU32(base, kStepEventHandle))) {
    event->Set(0, false);
  }
}

// Vblank thread: whether to hold a raise the game has decided on.
bool HoldStep() {
  static Clock::time_point held_since{};
  const Clock::time_point now = Clock::now();
  const auto close_hold = [&] {
    g_held_us.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - held_since).count()),
                        std::memory_order_relaxed);
    held_since = {};
  };
  if (held_since != Clock::time_point{} && !g_held.load(std::memory_order_acquire)) {
    // The swap raised the held step.
    close_hold();
  }
  bool hold = false;
  {
    std::lock_guard lock(g_gate_mutex);
    int64_t lead = StepLead();
    if (lead < 0) {
      // Frames swapped without a step (loading, paused, the main loop not waiting on the event): level again.
      g_gate_base += lead;
      lead = 0;
    }
    if (lead >= 2) {
      if (held_since == Clock::time_point{}) {
        held_since = now;
        g_holds.fetch_add(1, std::memory_order_relaxed);
      }
      const double max_hold = REXCVAR_GET(fh2_ritmo_passos_espera_max);
      if (std::chrono::duration<double, std::milli>(now - held_since).count() < max_hold) {
        hold = true;
      } else {
        // The render thread is not taking steps: run this one and count from the usual one step ahead.
        g_gate_base += lead - 1;
        g_timeouts.fetch_add(1, std::memory_order_relaxed);
      }
    }
    g_held.store(hold, std::memory_order_release);
  }
  if (!hold) {
    if (held_since != Clock::time_point{}) {
      close_hold();
    }
    g_raises.fetch_add(1, std::memory_order_relaxed);
  }
  return hold;
}

// Render thread, every VdSwap: steps per frame for the log, and the held raise.
void NoteSwap() {
  g_swaps.fetch_add(1, std::memory_order_release);
  bool raise = false;
  if (REXCVAR_GET(fh2_ritmo_passos)) {
    std::lock_guard lock(g_gate_mutex);
    if (g_held.load(std::memory_order_relaxed) && StepLead() < 2) {
      g_held.store(false, std::memory_order_release);
      raise = true;
    }
  }
  // Raise after unlocking: the vblank hook takes g_gate_mutex inside the guest interrupt, and the object lookup
  // takes the kernel's global lock, so raising under the mutex could deadlock the two threads (as in FH1).
  if (raise) {
    g_swap_raises.fetch_add(1, std::memory_order_relaxed);
    RaiseStepEvent();
  }

  static uint64_t last_steps = 0;
  static uint32_t frames[4] = {};
  static Clock::time_point report = Clock::now();
  const uint64_t steps = g_steps.load(std::memory_order_acquire);
  ++frames[std::min<uint64_t>(steps - last_steps, 3)];
  last_steps = steps;
  const Clock::time_point now = Clock::now();
  if (now - report >= std::chrono::seconds(10)) {
    const uint32_t total = frames[0] + frames[1] + frames[2] + frames[3];
    REXLOG_INFO(
        "[passos] gate={} frames={} 0={} 1={} ({:.1f} %) 2={} 3+={} | raises={} holds={} swap_raises={} "
        "timeouts={} held={:.1f} ms",
        REXCVAR_GET(fh2_ritmo_passos) ? "on" : "off", total, frames[0], frames[1],
        total ? 100.0 * frames[1] / total : 0.0, frames[2], frames[3], g_raises.exchange(0), g_holds.exchange(0),
        g_swap_raises.exchange(0), g_timeouts.exchange(0), g_held_us.exchange(0) / 1000.0);
    report = now;
    std::fill(std::begin(frames), std::end(frames), 0);
  }
}

const bool g_swap_observer_set = [] {
  rex::kernel::xboxkrnl::SetVdSwapObserver(&NoteSwap);
  return true;
}();

}  // namespace

// The game's vblank step decision (tail called by its vblank callback sub_825F5530, on the interrupt thread),
// replicated with the gate in front of the raise. Original:
//   ++count; settings = sub_824893E8();
//   raise = settings[+2403] ? count > 1 : (settings[+2402] && !byte[0x834241E9]) ? count > 1 : true;
//   if (raise) { sub_8248A468(handle) /* NtSetEvent */; count = 0; }
REX_HOOK_RAW(sub_8248BA70) {
  g_base.store(base, std::memory_order_release);
  if (!REXCVAR_GET(fh2_ritmo_passos)) {
    g_held.store(false, std::memory_order_release);
    __imp__sub_8248BA70(ctx, base);
    return;
  }
  const uint32_t count = LoadGuestU32(base, kStepVblankCount) + 1;
  StoreGuestU32(base, kStepVblankCount, count);
  sub_824893E8(ctx, base);
  const uint32_t settings = ctx.r3.u32;
  bool raise = true;
  if (base[settings + kSettingsEverySecond] != 0) {
    raise = count > 1;
  } else if (base[settings + kSettingsEverySecondUnless] != 0 && base[kStepEveryVblankOff] == 0) {
    raise = count > 1;
  }
  if (!raise || HoldStep()) {
    // A held raise keeps the count, so the next vblank asks again.
    return;
  }
  ctx.r3.u64 = LoadGuestU32(base, kStepEventHandle);
  sub_8248A468(ctx, base);
  StoreGuestU32(base, kStepVblankCount, 0);
}

// The wait for the step event in the main loop: one return, one step started.
REX_HOOK_RAW(sub_82475730) {
  const bool step_wait = static_cast<uint32_t>(ctx.lr) == kMainLoopWaitReturn;
  __imp__sub_82475730(ctx, base);
  if (step_wait) {
    g_steps.fetch_add(1, std::memory_order_release);
  }
}
