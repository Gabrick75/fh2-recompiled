/**
 * @file        rex/ui/surface_switch.h
 * @brief       Presentation surface over the default libnx NWindow
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/ui/surface.h>

// libnx; declared by tag so switch.h stays out of every UI header.
struct NWindow;

namespace rex::ui {

class Window;

class ViNWindowSurface final : public Surface {
 public:
  ViNWindowSurface(NWindow* window, const Window& owner) : window_(window), owner_(owner) {}

  TypeIndex GetType() const override { return kTypeIndex_ViNWindow; }
  NWindow* window() const { return window_; }

 protected:
  // The size the display wants (1280x720 handheld, the TV resolution docked),
  // as tracked by the owning window. It can differ from the NWindow's current
  // dimensions until the presenter recreates the swapchain.
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  NWindow* window_;
  const Window& owner_;
};

}  // namespace rex::ui
