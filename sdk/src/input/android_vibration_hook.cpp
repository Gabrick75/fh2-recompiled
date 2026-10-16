#include "rex/input/android_vibration_hook.h"

static void (*g_vibrate_cb)(int duration_ms, int intensity) = nullptr;
static bool (*g_enabled_cb)(void) = nullptr;
static const char* (*g_mode_cb)(void) = nullptr;

extern "C" void fx2_android_vibration_register_hooks(void (*vibrate)(int, int),
                                                     bool (*enabled)(void),
                                                     const char* (*mode)(void)) {
    g_vibrate_cb = vibrate;
    g_enabled_cb = enabled;
    g_mode_cb = mode;
}

extern "C" bool fx2_android_vibration_enabled(void) {
    return g_enabled_cb ? g_enabled_cb() : false;
}

extern "C" const char* fx2_android_vibration_mode(void) {
    return g_mode_cb ? g_mode_cb() : "both";
}

extern "C" void fx2_android_vibration_vibrate(int duration_ms, int intensity) {
    if (g_vibrate_cb) {
        g_vibrate_cb(duration_ms, intensity);
    }
}
