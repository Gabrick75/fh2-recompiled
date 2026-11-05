// The screen's aspect when the picture is stretched to it (fh2_tela_aspecto, app/src/fh2_tela.cpp), from
// GameActivity.resizeSurface: the world is then drawn with that aspect into the 16:9 image and is not distorted.
#include <jni.h>

#include <cmath>

#include <fmt/format.h>
#include <rex/cvar.h>

extern "C" JNIEXPORT void JNICALL Java_com_forzahorizon2_android_GameActivity_nativeSetScreenAspect(JNIEnv*, jclass,
                                                                                                 jfloat aspect) {
  if (!std::isfinite(aspect) || aspect < 0.0f) return;
  rex::cvar::SetFlagByName("fh2_tela_aspecto", fmt::format("{:.4f}", std::fmin(double(aspect), 8.0)));
}
