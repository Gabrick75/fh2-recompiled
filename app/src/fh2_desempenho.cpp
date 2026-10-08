// fh2 - game functions replaced for speed ("desempenho" means "performance").
//
// WAITING FOR ROOM IN THE COMMAND RING (sub_82B72A90)
//
// When the GPU command ring is full, the game's D3D waits for the command processor (CP) to move its read
// pointer: it SPINS, re-reading the read pointer copy in guest memory ([device+0x2B10]) and calling
// sub_82B72A90 on each turn (a short spin of priority hints, then the hang watchdog; it returns 0 when the
// pointer has not moved for too long). On the Xbox 360 that was cheap. Here the CP is the bottleneck, so the
// game's render thread spun the whole frame at 100 % of a core (simpleperf on the S23 Ultra, event menu:
// sub_823F94D0 + sub_82B72A90 were 15 % of all the app's samples), heating the phone into thermal throttling,
// which slows the CP and the GPU in turn.
//
// The hook: when the pointer has not moved since the last turn, sleep on a futex until the CP publishes a new
// one (rex_cp_rptr_seq, src/graphics/command_processor.cpp), at most 1 ms. The game learns of the progress as
// soon as before, without burning a core. Ported from NFSMW-Recompiled-Mobile (android/app/src/main/cpp/
// ganchos.cpp, where the same loop is sub_82597690 -> sub_825A5D18).
//
// Only for the four ring-room loops, told apart by the return address; the other callers of sub_82B72A90 wait
// for fences the CP writes mid-buffer, and sleeping there would add latency. Cvar fh2_espera_anel turns it off
// to compare.

#include <dlfcn.h>

#include <atomic>
#include <cstdint>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(fh2_espera_anel, true, "FH2",
                    "Sleep instead of spinning while the game waits for room in the GPU command ring "
                    "(app/src/fh2_desempenho.cpp)");

REX_EXTERN(__imp__sub_82B72A90);

namespace {

// Return addresses of the calls from the four loops that wait for the ring's read pointer to move.
constexpr uint32_t kRingWaitReturns[] = {0x823F9568, 0x82419164, 0x8241C9F8, 0x82B6EC3C};

// Inside the D3D device: pointer to the guest copy of the CP read pointer (lwz rX, 0x2B10(device)).
constexpr uint32_t kDeviceReadPointerCopy = 0x2B10;

uint32_t LoadGuestU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  __builtin_memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

struct ReadPointerSignal {
  std::atomic<uint32_t>* sequence = nullptr;
  std::atomic<uint32_t>* waiters = nullptr;
};

// The GPU plugin's counters, looked up once it is loaded.
const ReadPointerSignal* Signal() {
  static ReadPointerSignal signal;
  static std::atomic<bool> ready{false};
  if (!ready.load(std::memory_order_acquire)) {
    void* plugin = dlopen("librexgpu-xenos.so", RTLD_NOW | RTLD_NOLOAD);
    if (!plugin) {
      return nullptr;
    }
    signal.sequence = static_cast<std::atomic<uint32_t>*>(dlsym(plugin, "rex_cp_rptr_seq"));
    signal.waiters = static_cast<std::atomic<uint32_t>*>(dlsym(plugin, "rex_cp_rptr_waiters"));
    REXLOG_INFO("[fh2] espera do anel: {}", signal.sequence && signal.waiters
                                                 ? "dorme no futex do processador de comandos"
                                                 : "contador do plugin ausente, gira como no console");
    ready.store(true, std::memory_order_release);
  }
  return signal.sequence && signal.waiters ? &signal : nullptr;
}

bool IsRingWait(uint32_t return_address) {
  for (uint32_t address : kRingWaitReturns) {
    if (return_address == address) {
      return true;
    }
  }
  return false;
}

}  // namespace

REX_HOOK_RAW(sub_82B72A90) {
#if defined(__linux__)
  if (REXCVAR_GET(fh2_espera_anel) && IsRingWait(static_cast<uint32_t>(ctx.lr))) {
    if (const ReadPointerSignal* signal = Signal()) {
      // Sequence first, pointer second: if the CP publishes in between, the sequence has already changed and
      // the futex returns at once.
      signal->waiters->fetch_add(1, std::memory_order_seq_cst);
      const uint32_t before = signal->sequence->load(std::memory_order_seq_cst);
      // r3 points to the wait state: { device, ..., read pointer seen at +8 }.
      const uint32_t state = ctx.r3.u32;
      const uint32_t device = LoadGuestU32(base, state + 0);
      const uint32_t seen = LoadGuestU32(base, state + 8);
      const uint32_t read_pointer = LoadGuestU32(base, LoadGuestU32(base, device + kDeviceReadPointerCopy));
      if (read_pointer == seen) {
        timespec limit{0, 1000000};  // 1 ms
        syscall(SYS_futex, signal->sequence, FUTEX_WAIT_PRIVATE, before, &limit, nullptr, 0);
      }
      signal->waiters->fetch_sub(1, std::memory_order_seq_cst);
    }
  }
#endif
  __imp__sub_82B72A90(ctx, base);
}
