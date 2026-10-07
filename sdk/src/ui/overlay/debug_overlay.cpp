/**
 * @file        ui/overlay/debug_overlay.cpp
 *
 * @brief       Debug overlay implementation. See debug_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/platform.h>
#include <rex/version.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iomanip>
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
#include <rex/perf/counter.h>
#include <cinttypes>
#endif

namespace rex::ui {

namespace {
// Live debug overlays (queried by the Switch input driver).
std::atomic<int> g_debug_overlays{0};
}  // namespace

bool DebugOverlayAbierto() { return g_debug_overlays.load(std::memory_order_relaxed) > 0; }

DebugOverlayDialog::DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider)
    : ImGuiDialog(imgui_drawer), stats_provider_(std::move(stats_provider)) {
  g_debug_overlays.fetch_add(1, std::memory_order_relaxed);
}

DebugOverlayDialog::~DebugOverlayDialog() { g_debug_overlays.fetch_sub(1, std::memory_order_relaxed); }

// On the TV there is no touch screen to drag it with. With L and R held, the right stick moves it at
// the UI's pace, which repaints continuously while it is open. The position is restored when it opens
// and saved one second after it stops moving, also when it was dragged with a finger.
void DebugOverlayDialog::PositionBeforeBegin(ImGuiIO& io) {
  if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
    return;
  }
  if (!position_loaded_) {
    position_loaded_ = true;
    if (!position_file_.empty()) {
      std::ifstream f(position_file_);
      float x = 0.0f;
      float y = 0.0f;
      if (f >> x >> y && x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f) {
        position_x_ = saved_x_ = x;
        position_y_ = saved_y_ = y;
        position_known_ = true;
        position_apply_ = true;
      }
    }
  }
  // When the screen size changes (console <-> TV) it is put back at its fractional position.
  if (io.DisplaySize.x != display_w_ || io.DisplaySize.y != display_h_) {
    display_w_ = io.DisplaySize.x;
    display_h_ = io.DisplaySize.y;
    position_apply_ = position_known_;
  }
  const auto analog = [&io](ImGuiKey key) { return io.KeysData[key - ImGuiKey_NamedKey_BEGIN].AnalogValue; };
  const bool lr = ImGui::IsKeyDown(ImGuiKey_GamepadL1) && ImGui::IsKeyDown(ImGuiKey_GamepadR1);
  const float dx = analog(ImGuiKey_GamepadRStickRight) - analog(ImGuiKey_GamepadRStickLeft);
  const float dy = analog(ImGuiKey_GamepadRStickDown) - analog(ImGuiKey_GamepadRStickUp);
  moved_by_stick_ = false;
  if (lr && position_known_ && (dx != 0.0f || dy != 0.0f)) {
    // At full tilt, 80 % of the screen height per second, on both axes.
    const float paso = 0.8f * io.DisplaySize.y * io.DeltaTime;
    position_x_ += dx * paso / io.DisplaySize.x;
    position_y_ += dy * paso / io.DisplaySize.y;
    position_apply_ = true;
    moved_by_stick_ = true;
    last_move_time_ = ImGui::GetTime();
  }
  if (position_apply_) {
    position_apply_ = false;
    position_x_ = std::clamp(position_x_, 0.0f, 1.0f);
    position_y_ = std::clamp(position_y_, 0.0f, 1.0f);
    ImGui::SetNextWindowPos(ImVec2(position_x_ * io.DisplaySize.x, position_y_ * io.DisplaySize.y),
                            ImGuiCond_Always);
  }
}

void DebugOverlayDialog::PositionAfterBegin(ImGuiIO& io) {
  if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
    return;
  }
  // Fully inside the screen: when moving it, dragging it or switching from the console to the TV.
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  const float x = std::clamp(pos.x, 0.0f, std::max(0.0f, io.DisplaySize.x - size.x));
  const float y = std::clamp(pos.y, 0.0f, std::max(0.0f, io.DisplaySize.y - size.y));
  const bool fuera = x != pos.x || y != pos.y;
  if (!position_known_) {
    // First time without a saved position: take the current one, without writing anything.
    position_x_ = saved_x_ = x / io.DisplaySize.x;
    position_y_ = saved_y_ = y / io.DisplaySize.y;
    position_known_ = true;
    position_apply_ = fuera;
    return;
  }
  // ImGui rounds the position to the pixel: with the stick the fraction is kept, so that a gentle tilt
  // moves it too. Without the stick and more than one pixel from where it was placed, a finger has
  // dragged it.
  const bool arrastrada = !moved_by_stick_ && (std::abs(x - position_x_ * io.DisplaySize.x) > 1.0f ||
                                               std::abs(y - position_y_ * io.DisplaySize.y) > 1.0f);
  if (fuera || arrastrada) {
    position_x_ = x / io.DisplaySize.x;
    position_y_ = y / io.DisplaySize.y;
    position_apply_ = fuera;  // corrected on the next frame
  }
  if (arrastrada) {
    last_move_time_ = ImGui::GetTime();
  }
  if (!position_file_.empty() && ImGui::GetTime() - last_move_time_ > 1.0 &&
      (std::abs(position_x_ - saved_x_) > 1e-3f || std::abs(position_y_ - saved_y_) > 1e-3f)) {
    std::ofstream f(position_file_, std::ios::trunc);
    f << std::fixed << std::setprecision(4) << position_x_ << ' ' << position_y_ << '\n';
    // Even if the write fails: it is not retried every frame.
    saved_x_ = position_x_;
    saved_y_ = position_y_;
  }
}

void DebugOverlayDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
#if REX_PLATFORM_SWITCH
  PositionBeforeBegin(io);
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  ImGui::SetNextWindowSize(ImVec2(280, 280), ImGuiCond_FirstUseEver);
#else
  ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_FirstUseEver);
#endif
  ImGui::SetNextWindowBgAlpha(0.5f);
  // It only shows data: no navigation and no focus when it appears. On Switch
  // the menus are driven with the game controller, and opening this overlay
  // (L+R+Up) must not take the controller away while driving.
  if (ImGui::Begin("Debug##overlay", nullptr,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNavInputs |
                       ImGuiWindowFlags_NoFocusOnAppearing)) {
#if REX_PLATFORM_SWITCH
    PositionAfterBegin(io);
#endif
    if (stats_provider_) {
      auto stats = stats_provider_();
      if (stats.frame_count > 0) {
        ImGui::Text("Guest: %.1f FPS (%.2f ms)", stats.fps, stats.frame_time_ms);
      }
    }
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    ImGui::Separator();

    // Frame time graph
    auto ft_us = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFrameTimeUs);
    float ft_ms = static_cast<float>(ft_us) / 1000.0f;
    frame_time_history_[frame_history_idx_] = ft_ms;
    frame_history_idx_ = (frame_history_idx_ + 1) % kFrameHistorySize;
    ImGui::PlotLines("##ft", frame_time_history_.data(), kFrameHistorySize,
                     static_cast<int>(frame_history_idx_), "Frame (ms)", 0.0f, 50.0f,
                     ImVec2(200, 40));

    // GPU
    ImGui::Text("Draw: %" PRId64 "  Stalls: %" PRId64 "  Verts: %" PRId64,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kDrawCalls),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kCommandBufferStalls),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kVerticesProcessed));

    // Audio
    ImGui::Text("XMA: %" PRId64 "  Lat: %.1fms  BufQ: %" PRId64,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kXmaFramesDecoded),
                static_cast<float>(
                    rex::perf::GetSnapshotCounter(rex::perf::CounterId::kAudioFrameLatencyUs)) /
                    1000.0f,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kBufferQueueDepth));

    // Dispatch
    ImGui::Text("Dispatch: %" PRId64 "  IRQ: %" PRId64,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFunctionsDispatched),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kInterruptDispatches));

    // Threading
    ImGui::Text("Threads: %" PRId64 "  APC: %" PRId64 "  Contention: %" PRId64,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kActiveThreads),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kApcQueueDepth),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kCriticalRegionContentions));

    // Caches
    auto tex_h = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kTextureCacheHits);
    auto tex_m = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kTextureCacheMisses);
    auto pip_h = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kPipelineCacheHits);
    auto pip_m = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kPipelineCacheMisses);
    ImGui::Text("TexCache: %" PRId64 "/%" PRId64 "  PipeCache: %" PRId64 "/%" PRId64, tex_h,
                tex_h + tex_m, pip_h, pip_h + pip_m);
#endif
  }
  ImGui::End();

  // Build stamp watermark -- centered near bottom of screen
  auto text_size = ImGui::CalcTextSize(REXGLUE_BUILD_STAMP);
  float padding = ImGui::GetStyle().WindowPadding.x * 2.0f;
  float bottom_offset = io.DisplaySize.y * 0.03f;
  ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - text_size.x - padding) * 0.5f,
                                 io.DisplaySize.y - text_size.y - bottom_offset));
  ImGui::SetNextWindowSize(ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_Text, imgui_drawer()->style().debug.muted_text);
  if (ImGui::Begin("##watermark", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(REXGLUE_BUILD_STAMP);
  }
  ImGui::End();
  ImGui::PopStyleColor();
}

}  // namespace rex::ui
