#ifndef REX_INPUT_ANDROID_VIBRATION_HOOK_H
#define REX_INPUT_ANDROID_VIBRATION_HOOK_H

#ifdef __cplusplus
extern "C" {
#endif

/** Register callbacks for Android phone vibration. Called by the Android app at startup. */
void rex_android_vibration_register_hooks(void (*vibrate)(int duration_ms, int intensity),
                                          bool (*enabled)(void));

/** Call these from the SDL input driver when the gamepad rumbles. */
bool rex_android_vibration_enabled(void);
void rex_android_vibration_vibrate(int duration_ms, int intensity);

#ifdef __cplusplus
}
#endif

#endif // REX_INPUT_ANDROID_VIBRATION_HOOK_H
