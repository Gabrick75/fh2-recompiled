#ifndef REX_INPUT_ANDROID_VIBRATION_HOOK_H
#define REX_INPUT_ANDROID_VIBRATION_HOOK_H

#ifdef __cplusplus
extern "C" {
#endif

/** Register callbacks for Android phone vibration. Called by the Android app at startup. */
void fx2_android_vibration_register_hooks(void (*vibrate)(int duration_ms, int intensity),
                                          bool (*enabled)(void),
                                          const char* (*mode)(void));

/** Call these from the SDL input driver when the gamepad rumbles. */
bool fx2_android_vibration_enabled(void);
const char* fx2_android_vibration_mode(void);  // "phone", "gamepad", "both"
/** Vibrates the phone for duration_ms at intensity 1-100; intensity 0 stops it. */
void fx2_android_vibration_vibrate(int duration_ms, int intensity);

#ifdef __cplusplus
}
#endif

#endif // REX_INPUT_ANDROID_VIBRATION_HOOK_H
