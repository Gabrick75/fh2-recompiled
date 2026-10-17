#include <jni.h>
#include <android/log.h>
#include <pthread.h>

#include <string>

#include <rex/cvar.h>

#include "android_vibration.h"
#include "../../../../../sdk/include/rex/input/android_vibration_hook.h"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "FH2Vibration", __VA_ARGS__)

// The launcher's "Vibração" options come as arguments (GameOptions.arguments), like every other option: the
// game asks for them on every XInputSetState (once per frame), so they must cost a variable read, not a trip
// through the JVM.
REXCVAR_DEFINE_BOOL(fh2_android_vibracao, true, "FH2",
                    "Android: the phone vibrates when the game rumbles (the launcher's Vibração toggle)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(fh2_android_vibracao_modo, "both", "FH2",
                      "Android: what vibrates when the game rumbles: phone, gamepad or both")
    .allowed({"phone", "gamepad", "both"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

JavaVM* g_vm = nullptr;

// Looked up once (JNI_OnLoad, on a Java thread): the Vibrator service and the methods vibrate_via_java calls.
jobject g_vibrator = nullptr;  // global ref
jclass g_vibration_effect = nullptr;  // global ref, null before API 26
jmethodID g_create_one_shot = nullptr;
jmethodID g_vibrate_effect = nullptr;
jmethodID g_vibrate_ms = nullptr;

// Threads this file attached to the JVM are detached when they exit, not after every call: an attach creates a
// java.lang.Thread each time. Threads that were already attached (Java's, SDL's) are never detached here.
pthread_key_t g_detach_key;
pthread_once_t g_detach_once = PTHREAD_ONCE_INIT;

void DetachAtExit(void*) {
  if (g_vm) {
    g_vm->DetachCurrentThread();
  }
}

void CreateDetachKey() { pthread_key_create(&g_detach_key, DetachAtExit); }

JNIEnv* EnvForThisThread() {
  if (!g_vm) {
    return nullptr;
  }
  JNIEnv* env = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
    return env;
  }
  if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
    return nullptr;
  }
  pthread_once(&g_detach_once, CreateDetachKey);
  pthread_setspecific(g_detach_key, env);
  return env;
}

bool ClearException(JNIEnv* env) {
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return true;
  }
  return false;
}

void LookUpVibrator(JNIEnv* env) {
  jclass activity_thread = env->FindClass("android/app/ActivityThread");
  jmethodID current_application =
      activity_thread ? env->GetStaticMethodID(activity_thread, "currentApplication", "()Landroid/app/Application;")
                      : nullptr;
  jobject application = current_application ? env->CallStaticObjectMethod(activity_thread, current_application)
                                            : nullptr;
  jclass context = env->FindClass("android/content/Context");
  jmethodID get_system_service =
      context ? env->GetMethodID(context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : nullptr;
  jstring service = env->NewStringUTF("vibrator");  // Context.VIBRATOR_SERVICE
  jobject vibrator = (application && get_system_service)
                         ? env->CallObjectMethod(application, get_system_service, service)
                         : nullptr;
  jclass vibrator_class = env->FindClass("android/os/Vibrator");
  if (ClearException(env) || !vibrator || !vibrator_class) {
    LOGE("Vibrator service not available: the phone will not vibrate");
    return;
  }
  g_vibrate_ms = env->GetMethodID(vibrator_class, "vibrate", "(J)V");
  ClearException(env);
  jclass effect = env->FindClass("android/os/VibrationEffect");
  if (!ClearException(env) && effect) {
    g_create_one_shot = env->GetStaticMethodID(effect, "createOneShot", "(JI)Landroid/os/VibrationEffect;");
    g_vibrate_effect = env->GetMethodID(vibrator_class, "vibrate", "(Landroid/os/VibrationEffect;)V");
    if (!ClearException(env) && g_create_one_shot && g_vibrate_effect) {
      g_vibration_effect = static_cast<jclass>(env->NewGlobalRef(effect));
    }
  }
  g_vibrator = env->NewGlobalRef(vibrator);
}

void vibrate_via_java(int duration_ms, int intensity) {
  if (!g_vibrator) {
    return;
  }
  JNIEnv* env = EnvForThisThread();
  if (!env) {
    return;
  }
  // amplitude 1-255 from intensity 0-100
  int amplitude = intensity * 255 / 100;
  if (amplitude < 1) amplitude = 1;
  if (amplitude > 255) amplitude = 255;
  // Local refs pile up on a native thread that never returns to Java: free them here.
  if (env->PushLocalFrame(4) != JNI_OK) {
    ClearException(env);
    return;
  }
  bool done = false;
  if (g_vibration_effect) {
    jobject effect =
        env->CallStaticObjectMethod(g_vibration_effect, g_create_one_shot, jlong(duration_ms), jint(amplitude));
    if (!ClearException(env) && effect) {
      env->CallVoidMethod(g_vibrator, g_vibrate_effect, effect);
      done = !ClearException(env);
    }
  }
  if (!done && g_vibrate_ms) {
    env->CallVoidMethod(g_vibrator, g_vibrate_ms, jlong(duration_ms));
    ClearException(env);
  }
  env->PopLocalFrame(nullptr);
}

bool vibration_enabled() { return REXCVAR_GET(fh2_android_vibracao); }

// One of three literals: never a pointer into a string that can change.
const char* vibration_mode() {
  static const char* const mode = [] {
    const std::string& value = REXCVAR_GET(fh2_android_vibracao_modo);
    return value == "phone" ? "phone" : value == "gamepad" ? "gamepad" : "both";
  }();
  return mode;
}

void app_vibrate_cb(int duration_ms, int intensity) { vibrate_via_java(duration_ms, intensity); }

bool app_vibration_enabled_cb(void) { return vibration_enabled(); }

const char* app_vibration_mode_cb(void) { return vibration_mode(); }

}  // namespace

extern "C" jint JNI_OnLoad(JavaVM* vm, void*) {
  g_vm = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env) {
    LookUpVibrator(env);
  }
  fx2_android_vibration_register_hooks(app_vibrate_cb, app_vibration_enabled_cb, app_vibration_mode_cb);
  return JNI_VERSION_1_6;
}

extern "C" bool fh2_android_vibration_enabled(void) { return vibration_enabled(); }

extern "C" const char* fh2_android_vibration_mode(void) { return vibration_mode(); }

extern "C" void fh2_android_vibrate(int durationMs, int intensity) { vibrate_via_java(durationMs, intensity); }

// Keep the original JNI entry point for direct Java calls too.
extern "C" JNIEXPORT void JNICALL
Java_com_forzahorizon2_android_TouchControlsView_nativeVibrate(JNIEnv*, jclass, jint durationMs, jint intensity) {
  vibrate_via_java(durationMs, intensity);
}
