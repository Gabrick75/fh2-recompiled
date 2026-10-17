#include <jni.h>
#include <android/log.h>
#include <string>

#include "android_vibration.h"
#include "../../../../../sdk/include/rex/input/android_vibration_hook.h"

#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, "FH2Vibration", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "FH2Vibration", __VA_ARGS__)

static JavaVM* g_vm = nullptr;

static void vibrate_via_java(int durationMs, int intensity);
static bool vibration_enabled_via_java(void);
static const char* vibration_mode_via_java(void);

static void app_vibrate_cb(int duration_ms, int intensity) {
    vibrate_via_java(duration_ms, intensity);
}

static bool app_vibration_enabled_cb(void) {
    return vibration_enabled_via_java();
}

static const char* app_vibration_mode_cb(void) {
    return vibration_mode_via_java();
}

extern "C" jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    fx2_android_vibration_register_hooks(app_vibrate_cb, app_vibration_enabled_cb,
                                         app_vibration_mode_cb);
    LOGV("JNI_OnLoad: stored JavaVM and registered vibration hooks");
    return JNI_VERSION_1_6;
}

static void vibrate_via_java(int durationMs, int intensity) {
    LOGE("vibrate_via_java: duration=%d intensity=%d g_vm=%p", durationMs, intensity,
         (void*)g_vm);
    if (!g_vm) {
        LOGE("JavaVM not available");
        return;
    }

    JNIEnv* env = nullptr;
    jint rs = g_vm->AttachCurrentThread(&env, nullptr);
    if (rs != JNI_OK || !env) {
        LOGE("could not attach thread to JVM");
        return;
    }

    // amplitude 0-255 from intensity 0-100
    int amplitude = (int)(((double)intensity / 100.0) * 255.0);
    if (amplitude < 0) amplitude = 0;
    if (amplitude > 255) amplitude = 255;

    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if (!activityThread) {
        LOGE("could not find ActivityThread");
        g_vm->DetachCurrentThread();
        return;
    }

    jmethodID currentApplication = env->GetStaticMethodID(activityThread, "currentApplication",
                                                          "()Landroid/app/Application;");
    if (!currentApplication) {
        LOGE("could not find currentApplication");
        g_vm->DetachCurrentThread();
        return;
    }

    jobject application = env->CallStaticObjectMethod(activityThread, currentApplication);
    if (!application) {
        LOGE("could not get Application");
        g_vm->DetachCurrentThread();
        return;
    }

    jclass context = env->FindClass("android/content/Context");
    if (!context) {
        LOGE("could not find Context");
        g_vm->DetachCurrentThread();
        return;
    }

    jfieldID vibratorServiceField = env->GetStaticFieldID(context, "VIBRATOR_SERVICE",
                                                          "Ljava/lang/String;");
    if (!vibratorServiceField) {
        LOGE("could not find VIBRATOR_SERVICE");
        g_vm->DetachCurrentThread();
        return;
    }

    jstring vibratorService = (jstring)env->GetStaticObjectField(context, vibratorServiceField);
    if (!vibratorService) {
        LOGE("could not get VIBRATOR_SERVICE string");
        g_vm->DetachCurrentThread();
        return;
    }

    jmethodID getSystemService = env->GetMethodID(context, "getSystemService",
                                                  "(Ljava/lang/String;)Ljava/lang/Object;");
    if (!getSystemService) {
        LOGE("could not find getSystemService");
        g_vm->DetachCurrentThread();
        return;
    }

    jobject vibrator = env->CallObjectMethod(application, getSystemService, vibratorService);
    if (!vibrator) {
        LOGE("could not get Vibrator service");
        g_vm->DetachCurrentThread();
        return;
    }

    jclass vibratorClass = env->FindClass("android/os/Vibrator");
    if (!vibratorClass) {
        LOGE("could not find Vibrator class");
        g_vm->DetachCurrentThread();
        return;
    }

    bool usedEffect = false;
    jclass vibrationEffectClass = env->FindClass("android/os/VibrationEffect");
    if (vibrationEffectClass) {
        jmethodID createOneShot = env->GetStaticMethodID(vibrationEffectClass, "createOneShot",
                                                         "(JI)Landroid/os/VibrationEffect;");
        if (createOneShot) {
            jobject effect = env->CallStaticObjectMethod(vibrationEffectClass, createOneShot,
                                                         (jlong)durationMs, (jint)amplitude);
            if (effect) {
                jmethodID vibrate = env->GetMethodID(vibratorClass, "vibrate",
                                                     "(Landroid/os/VibrationEffect;)V");
                if (vibrate) {
                    env->CallVoidMethod(vibrator, vibrate, effect);
                    LOGV("vibrate(VibrationEffect) duration=%d amplitude=%d", durationMs, amplitude);
                    usedEffect = true;
                } else {
                    LOGE("could not find Vibrator.vibrate(VibrationEffect)");
                }
            } else {
                LOGE("could not create VibrationEffect");
            }
        } else {
            LOGE("could not find VibrationEffect.createOneShot");
        }
    }

    if (!usedEffect) {
        jmethodID vibrateMs = env->GetMethodID(vibratorClass, "vibrate", "(J)V");
        if (vibrateMs) {
            env->CallVoidMethod(vibrator, vibrateMs, (jlong)durationMs);
            LOGV("vibrate(ms) duration=%d", durationMs);
        } else {
            LOGE("could not find Vibrator.vibrate(long)");
        }
    }

    g_vm->DetachCurrentThread();
}

static bool vibration_enabled_via_java(void) {
    LOGV("vibration_enabled_via_java: called");
    if (!g_vm) {
        LOGV("vibration_enabled_via_java: no g_vm");
        return false;
    }

    JNIEnv* env = nullptr;
    jint rs = g_vm->AttachCurrentThread(&env, nullptr);
    if (rs != JNI_OK || !env) {
        return false;
    }

    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if (!activityThread) {
        g_vm->DetachCurrentThread();
        return false;
    }

    jmethodID currentApplication = env->GetStaticMethodID(activityThread, "currentApplication",
                                                          "()Landroid/app/Application;");
    if (!currentApplication) {
        g_vm->DetachCurrentThread();
        return false;
    }

    jobject application = env->CallStaticObjectMethod(activityThread, currentApplication);
    if (!application) {
        g_vm->DetachCurrentThread();
        return false;
    }

    jclass context = env->FindClass("android/content/Context");
    if (!context) {
        g_vm->DetachCurrentThread();
        return false;
    }

    jmethodID getSharedPreferences = env->GetMethodID(context, "getSharedPreferences",
                                                      "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");
    if (!getSharedPreferences) {
        g_vm->DetachCurrentThread();
        return false;
    }

    jstring prefsName = env->NewStringUTF("fh2_game");
    jobject prefs = env->CallObjectMethod(application, getSharedPreferences, prefsName, 0);
    env->DeleteLocalRef(prefsName);
    if (!prefs) {
        g_vm->DetachCurrentThread();
        return false;
    }

    jclass prefsClass = env->FindClass("android/content/SharedPreferences");
    if (!prefsClass) {
        env->DeleteLocalRef(prefs);
        g_vm->DetachCurrentThread();
        return false;
    }

    jmethodID getBoolean = env->GetMethodID(prefsClass, "getBoolean",
                                            "(Ljava/lang/String;Z)Z");
    if (!getBoolean) {
        env->DeleteLocalRef(prefs);
        g_vm->DetachCurrentThread();
        return false;
    }

    jstring key = env->NewStringUTF("vibration_phone");
    jboolean enabled = env->CallBooleanMethod(prefs, getBoolean, key, JNI_TRUE);
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(prefs);
    g_vm->DetachCurrentThread();

    return enabled == JNI_TRUE;
}

static std::string vibration_mode_via_java() {
    std::string mode = "both";
    if (!g_vm) {
        return mode;
    }

    JNIEnv* env = nullptr;
    jint rs = g_vm->AttachCurrentThread(&env, nullptr);
    if (rs != JNI_OK || !env) {
        return mode;
    }

    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if (!activityThread) {
        g_vm->DetachCurrentThread();
        return mode;
    }

    jmethodID currentApplication = env->GetStaticMethodID(activityThread, "currentApplication",
                                                          "()Landroid/app/Application;");
    if (!currentApplication) {
        g_vm->DetachCurrentThread();
        return mode;
    }

    jobject application = env->CallStaticObjectMethod(activityThread, currentApplication);
    if (!application) {
        g_vm->DetachCurrentThread();
        return mode;
    }

    jclass context = env->FindClass("android/content/Context");
    if (!context) {
        g_vm->DetachCurrentThread();
        return mode;
    }

    jmethodID getSharedPreferences = env->GetMethodID(context, "getSharedPreferences",
                                                      "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");
    if (!getSharedPreferences) {
        g_vm->DetachCurrentThread();
        return mode;
    }

    jstring prefsName = env->NewStringUTF("fh2_game");
    jobject prefs = env->CallObjectMethod(application, getSharedPreferences, prefsName, 0);
    env->DeleteLocalRef(prefsName);
    if (!prefs) {
        g_vm->DetachCurrentThread();
        return mode;
    }

    jclass prefsClass = env->FindClass("android/content/SharedPreferences");
    if (!prefsClass) {
        env->DeleteLocalRef(prefs);
        g_vm->DetachCurrentThread();
        return mode;
    }

    jmethodID getString = env->GetMethodID(prefsClass, "getString",
                                           "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (!getString) {
        env->DeleteLocalRef(prefs);
        g_vm->DetachCurrentThread();
        return mode;
    }

    jstring key = env->NewStringUTF("vibration_mode");
    jstring def = env->NewStringUTF("both");
    jstring result = (jstring)env->CallObjectMethod(prefs, getString, key, def);
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(def);
    env->DeleteLocalRef(prefs);
    g_vm->DetachCurrentThread();

    if (result) {
        const char* chars = env->GetStringUTFChars(result, nullptr);
        if (chars) {
            mode = chars;
            env->ReleaseStringUTFChars(result, chars);
        }
        env->DeleteLocalRef(result);
    }

    return mode;
}

extern "C" bool fh2_android_vibration_enabled(void) {
    return vibration_enabled_via_java();
}

extern "C" const char* fh2_android_vibration_mode(void) {
    static std::string cached = vibration_mode_via_java();
    return cached.c_str();
}

extern "C" void fh2_android_vibrate(int durationMs, int intensity) {
    vibrate_via_java(durationMs, intensity);
}

// Keep the original JNI entry point for direct Java calls too.
extern "C" JNIEXPORT void JNICALL
Java_com_forzahorizon2_android_TouchControlsView_nativeVibrate(JNIEnv* env, jclass, jint durationMs,
                                                               jint intensity) {
    vibrate_via_java(durationMs, intensity);
}
