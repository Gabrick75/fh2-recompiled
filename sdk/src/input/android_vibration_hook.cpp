#include "rex/input/android_vibration_hook.h"

static void (*g_vibrate_cb)(int duration_ms, int intensity) = nullptr;
static bool (*g_enabled_cb)(void) = nullptr;
static bool (*g_with_gamepad_cb)(void) = nullptr;

extern "C" void rex_android_vibration_register_hooks(void (*vibrate)(int, int),
                                                     bool (*enabled)(void),
                                                     bool (*with_gamepad)(void)) {
    g_vibrate_cb = vibrate;
    g_enabled_cb = enabled;
    g_with_gamepad_cb = with_gamepad;
}

extern "C" bool rex_android_vibration_enabled(void) {
    return g_enabled_cb ? g_enabled_cb() : false;
}

extern "C" bool rex_android_vibration_with_gamepad(void) {
    return g_with_gamepad_cb ? g_with_gamepad_cb() : false;
}

extern "C" void rex_android_vibration_vibrate(int duration_ms, int intensity) {
    if (g_vibrate_cb) {
        g_vibrate_cb(duration_ms, intensity);
    }
}
