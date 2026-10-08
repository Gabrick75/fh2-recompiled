#pragma once
// Where the command processor's time goes, per frame ("tempos"), written to the log next to the "fps:" line by
// the Vulkan command processor. Adapted from NFSMW-Recompiled-Mobile (tools/parche_tiempos.py).
//
// idle, wait_reg, draws, copies and swap do not overlap (different packets or parts of the loop); gpu_wait and
// submit fall inside them and are reported as "of which".

#include <atomic>
#include <chrono>
#include <cstdint>

namespace rex::graphics {

struct CpTimes {
  std::atomic<uint64_t> idle_ns{0};      // ring empty, nothing to execute
  std::atomic<uint64_t> wait_reg_ns{0};  // WAIT_REG_MEM not met yet
  std::atomic<uint32_t> wait_regs{0};
  std::atomic<uint64_t> draw_ns{0};      // IssueDraw
  std::atomic<uint32_t> draws{0};
  std::atomic<uint64_t> copy_ns{0};      // IssueCopy (resolves)
  std::atomic<uint32_t> copies{0};
  std::atomic<uint64_t> swap_ns{0};      // IssueSwap
  std::atomic<uint64_t> gpu_wait_ns{0};  // vkWaitForFences
  std::atomic<uint32_t> gpu_waits{0};
  std::atomic<uint64_t> submit_ns{0};    // vkQueueSubmit
  std::atomic<uint32_t> submits{0};
  // Predicated tiling: SET_BIN_SELECT packets (one per tile pass) and predicated packets skipped because they
  // do not touch the selected tile.
  std::atomic<uint32_t> bin_selects{0};
  std::atomic<uint32_t> predicated_skipped{0};
  // GPU work that does not shrink with gpu_escala_render: resolve copies (dump + copy compute at 1x), and
  // textures loaded from guest memory (untiling compute).
  std::atomic<uint64_t> resolve_pixels{0};
  std::atomic<uint32_t> texture_loads{0};
  std::atomic<uint64_t> texture_load_pixels{0};
  // Debug (gpu_log_quadros): frames left to log every resolve, texture load and swap in order, with "[quadro]".
  std::atomic<int32_t> frames_to_log{0};
  // Resolve shortcut (gpu_atalho_resolve): resolves taken by it, textures filled from it, and shortcuts lost
  // to an unexpected reader.
  std::atomic<uint32_t> atalho_resolves{0};
  std::atomic<uint32_t> atalho_hits{0};
  std::atomic<uint32_t> atalho_misses{0};
  // Ownership transfers between render targets: calls with transfers or clears, and transfer draws.
  std::atomic<uint32_t> transfer_calls{0};
  std::atomic<uint32_t> transfer_draws{0};
};

extern CpTimes g_cp_times;

// With the gpu_medir_pacotes cvar: exclusive time per PM4 packet kind (nested INDIRECT_BUFFER packets are not
// counted twice). 0-127 type 3 packets by opcode, 128 type 0 (register writes), 129 types 1 and 2. Command
// processor thread only, which also prints them.
constexpr int kCpPacketKinds = 130;
extern uint64_t g_cp_packet_ns[kCpPacketKinds];
extern uint32_t g_cp_packet_count[kCpPacketKinds];

// Adds its lifetime to 'total'.
class CpTimer {
 public:
  explicit CpTimer(std::atomic<uint64_t>& total) : total_(total) {}
  ~CpTimer() {
    total_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - start_)
                                  .count()),
                     std::memory_order_relaxed);
  }
  CpTimer(const CpTimer&) = delete;
  CpTimer& operator=(const CpTimer&) = delete;

 private:
  std::atomic<uint64_t>& total_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

}  // namespace rex::graphics
