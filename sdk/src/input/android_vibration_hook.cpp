#include "rex/input/android_vibration_hook.h"

namespace rex::input {

static void (*g_vibrate_cb)(int duration_ms, int intensity) = nullptr;
static bool (*g_enabled_cb)(void) = nullptr;

void rex_android_vibration_register_hooks(void (*vibrate)(int, int),
                                          bool (*enabled)(void)) {
    g_vibrate_cb = vibrate;
    g_enabled_cb = enabled;
}

bool rex_android_vibration_enabled(void) {
    return g_enabled_cb ? g_enabled_cb() : false;
}

void rex_android_vibration_vibrate(int duration_ms, int intensity) {
    if (g_vibrate_cb) {
        g_vibrate_cb(duration_ms, intensity);
    }
}

} // namespace rex::input
