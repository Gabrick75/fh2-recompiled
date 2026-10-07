/**
 * @file        rex/ui/window_switch.h
 * @brief       Window for Nintendo Switch: the fullscreen default NWindow
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <memory>
#include <string_view>

#include <rex/ui/window.h>

struct NWindow;

namespace rex::ui {

class SwitchWindowedAppContext;

class WindowSwitch final : public Window {
 public:
  WindowSwitch(WindowedAppContext& app_context, const std::string_view title,
               uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~WindowSwitch() override;

  // The resolution the display wants now: 1280x720 in handheld mode, the TV
  // output resolution when docked.
  static void QueryDisplayResolution(uint32_t& width_out, uint32_t& height_out);

  // Called by SwitchWindowedAppContext on the UI thread.
  void HandleDisplayModeChange();
  void HandleFocusChange(bool has_focus);
  void HandlePendingPaint();
  void HandleCloseRequest();

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;

  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  // May be called from non-UI threads.
  void RequestPaintImpl() override;

 private:
  SwitchWindowedAppContext& switch_app_context() const;
  void PerformClose();

  NWindow* nwindow_ = nullptr;
  bool has_focus_ = false;
  std::atomic<bool> paint_pending_{false};
};

}  // namespace rex::ui
