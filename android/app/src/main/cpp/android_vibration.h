#ifndef FH2_ANDROID_VIBRATION_H
#define FH2_ANDROID_VIBRATION_H

#ifdef __cplusplus
extern "C" {
#endif

/** Called by the SDK when the gamepad rumbles. Makes the Android phone vibrate
 *  with the given duration (ms) and intensity (0-100), if the user enabled it. */
void fh2_android_vibrate(int durationMs, int intensity);

/** Returns true if the user enabled phone vibration in the launcher (VIBRATION_PHONE). */
bool fh2_android_vibration_enabled(void);

/** Returns the vibration mode: "phone", "gamepad", or "both". */
const char* fh2_android_vibration_mode(void);

#ifdef __cplusplus
}
#endif

#endif // FH2_ANDROID_VIBRATION_H
