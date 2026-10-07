#include <rex/ui/surface_android.h>

#include <SDL3/SDL_video.h>

namespace rex::ui {

AndroidNativeWindowSurface::AndroidNativeWindowSurface(ANativeWindow* window,
                                                       SDL_Window* sdl_window)
    : window_(window), sdl_window_(sdl_window) {
  if (window_) {
    ANativeWindow_acquire(window_);
  }
}

AndroidNativeWindowSurface::~AndroidNativeWindowSurface() {
  if (window_) {
    ANativeWindow_release(window_);
  }
}

bool AndroidNativeWindowSurface::GetSizeImpl(uint32_t& width_out,
                                             uint32_t& height_out) const {
  int width = 0;
  int height = 0;
  if (!sdl_window_ || !SDL_GetWindowSizeInPixels(sdl_window_, &width, &height) ||
      width <= 0 || height <= 0) {
    width_out = 0;
    height_out = 0;
    return false;
  }
  width_out = static_cast<uint32_t>(width);
  height_out = static_cast<uint32_t>(height);
  return true;
}

}  // namespace rex::ui
