// Relative HDR (present_hdr_relative, sdk/src/ui/vulkan/vulkan_presenter.cpp): the display's current HDR/SDR ratio,
// which Android changes with the brightness and the ambient light, from GameActivity's listener to the presenter.
#include <jni.h>

#include <cmath>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>

extern "C" JNIEXPORT void JNICALL Java_com_forzahorizon2_android_GameActivity_nativeSetHdrHeadroom(JNIEnv*, jclass,
                                                                                                jfloat ratio) {
  if (!std::isfinite(ratio)) return;
  const double value = std::fmin(std::fmax(double(ratio), 1.0), 100.0);
  rex::cvar::SetFlagByName("present_hdr_headroom", fmt::format("{:.4f}", value));
}
