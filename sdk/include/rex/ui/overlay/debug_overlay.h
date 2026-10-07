/**
 * @file        rex/ui/overlay/debug_overlay.h
 *
 * @brief       ImGui debug overlay dialog for frame timing display.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/ui/imgui_dialog.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace rex::ui {

struct FrameStats {
  double frame_time_ms = 0;
  double fps = 0;
  uint64_t frame_count = 0;
};

// true while the debug overlay exists. On Switch, with it open, L+R and the right stick move it, and
// the input driver does not pass L, R or that stick to the game while L and R are held.
bool DebugOverlayAbierto();

class DebugOverlayDialog : public ImGuiDialog {
 public:
  using FrameStatsProvider = std::function<FrameStats()>;

  explicit DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider = {});
  ~DebugOverlayDialog();

  void SetStatsProvider(FrameStatsProvider provider) { stats_provider_ = std::move(provider); }
  // File where the position is saved (on Switch it is restored when the overlay opens).
  void SetPositionFile(std::filesystem::path path) { position_file_ = std::move(path); }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Move with L+R and the right stick, and remember the position (only called on Switch).
  void PositionBeforeBegin(ImGuiIO& io);
  void PositionAfterBegin(ImGuiIO& io);

  FrameStatsProvider stats_provider_;
  // Position as a fraction of the screen, so it works the same on the TV and on the handheld screen.
  std::filesystem::path position_file_;
  bool position_loaded_ = false;
  bool position_known_ = false;
  bool position_apply_ = false;
  bool moved_by_stick_ = false;
  float display_w_ = 0.0f;
  float display_h_ = 0.0f;
  float position_x_ = 0.0f;
  float position_y_ = 0.0f;
  float saved_x_ = 0.0f;
  float saved_y_ = 0.0f;
  double last_move_time_ = 0.0;
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  static constexpr size_t kFrameHistorySize = 120;
  std::array<float, kFrameHistorySize> frame_time_history_{};
  size_t frame_history_idx_ = 0;
#endif
};

}  // namespace rex::ui
