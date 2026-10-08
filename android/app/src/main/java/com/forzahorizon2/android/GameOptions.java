package com.forzahorizon2.android;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.List;

/**
 * The launcher's settings (the menu follows NFSMW-Recompiled-Mobile's). Most of them become game cvars that
 * GameActivity passes on the command line, which wins over fh2.toml; the display ones are applied by GameActivity
 * itself. Some are only stored for now: their option row says so (see MainActivity.notYet).
 *
 * Forza Horizon 2 runs on the emulated Xenos GPU (there is no native renderer yet), so the fixed arguments are the
 * ones nfsmw-android found the emulation needs on Android drivers.
 */
final class GameOptions {
    private static final String PREFS = "fh2_game";

    // Display
    static final String RESOLUTION = "resolution";          // "480p", "720p", "native": size of the presented buffer
    static final String SHOW_FPS = "show_fps";
    // Graphics
    static final String RENDER_RESOLUTION = "render_resolution";  // not applied yet
    static final String FPS = "fps";                          // only picks the display refresh rate for now
    static final String ANTI_ALIASING = "anti_aliasing";      // "off", "msaa", "fxaa"
    static final String MSAA_SAMPLES = "msaa_samples";        // "2", "4"
    static final String FXAA_QUALITY = "fxaa_quality";        // "normal", "high"
    static final String SCALING = "scaling";                  // not applied yet (no FidelityFX on Android)
    static final String ANISOTROPIC = "anisotropic";          // anisotropic_override: 0, 2 (2x), 3 (4x), 5 (16x)
    static final String SHADOWS = "shadows";                  // not applied yet (needs game hooks)
    static final String SHADOW_DISTANCE = "shadow_distance";  // not applied yet
    static final String CAR_REFLECTIONS = "car_reflections";  // not applied yet
    // Performance
    static final String PIN_THREADS = "pin_threads";
    static final String DRIVER = "driver";                    // imported driver name, "" = system; not loaded yet
    static final String TURBO = "turbo";                      // not applied yet
    // Sound
    static final String VOLUME = "volume";

    static final String RES_480P = "480p";
    static final String RES_720P = "720p";
    static final String RES_NATIVE = "native";

    private GameOptions() {
    }

    static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static String defaultOf(String key) {
        switch (key) {
            case RESOLUTION: return RES_NATIVE;
            case RENDER_RESOLUTION: return "1280x720";
            case FPS: return "60";
            // The game's own MSAA 4x: without it shadows and reflections show artefacts.
            case ANTI_ALIASING: return "msaa";
            case MSAA_SAMPLES: return "4";
            case FXAA_QUALITY: return "normal";
            case SCALING: return "bilinear";
            case ANISOTROPIC: return "3";
            case SHADOWS: return "1";
            case SHADOW_DISTANCE: return "far";
            case CAR_REFLECTIONS: return "high";
            case DRIVER: return "";
            case VOLUME: return "100";
            default: throw new IllegalArgumentException(key);
        }
    }

    static String get(Context context, String key) {
        return prefs(context).getString(key, defaultOf(key));
    }

    static void set(Context context, String key, String value) {
        prefs(context).edit().putString(key, value).apply();
    }

    static boolean flag(Context context, String key) {
        boolean byDefault = PIN_THREADS.equals(key);
        return prefs(context).getBoolean(key, byDefault);
    }

    static void flag(Context context, String key, boolean value) {
        prefs(context).edit().putBoolean(key, value).apply();
    }

    // Stretch is shared with the in-game controls panel, which keeps it with the touch controls.
    static boolean stretch(Context context) {
        return context.getSharedPreferences("fh2_controls", Context.MODE_PRIVATE).getBoolean("stretch", true);
    }

    static void stretch(Context context, boolean value) {
        context.getSharedPreferences("fh2_controls", Context.MODE_PRIVATE).edit().putBoolean("stretch", value).apply();
    }

    /** Width x height of the presented buffer, or {0, 0} for the view's own size. */
    static int[] bufferSize(String resolution) {
        if (RES_480P.equals(resolution)) return new int[] {720, 480};
        if (RES_720P.equals(resolution)) return new int[] {1280, 720};
        return new int[] {0, 0};
    }

    static List<String> arguments(Context context) {
        List<String> args = new ArrayList<>();
        args.add("--audio_ganancia_pct=" + get(context, VOLUME));
        switch (get(context, ANTI_ALIASING)) {
            case "off":
                args.add("--gpu_sin_msaa=true");
                break;
            case "fxaa":
                args.add("--gpu_sin_msaa=true");
                args.add("--swap_post_effect=" + ("high".equals(get(context, FXAA_QUALITY)) ? "fxaa_extreme" : "fxaa"));
                break;
            default:
                args.add("--gpu_msaa_muestras=" + get(context, MSAA_SAMPLES));
                break;
        }
        args.add("--anisotropic_override=" + get(context, ANISOTROPIC));
        args.add("--fh2_android_nucleos_grandes=" + flag(context, PIN_THREADS));
        // Emulated Xenos GPU on Android drivers (from nfsmw-android's "xenos" mode): ordinary descriptor sets,
        // host framebuffers, no geometry shader or non-solid fill required, shaders compiled before drawing.
        args.add("--gpu_plugin=xenos");
        args.add("--vulkan_native_shader_features=false");
        args.add("--render_target_path_vulkan=fbo");
        args.add("--vulkan_require_geometry_shader=false");
        args.add("--vulkan_require_fill_mode_non_solid=false");
        args.add("--async_shader_compilation=false");
        // Forza Horizon 2 leaves vertex fetch constants with type 0 in some draws; without this the emulated GPU
        // drops them (same as tools/rodar_pc.sh on the PC).
        args.add("--gpu_allow_invalid_fetch_constants=true");
        // The view gives the aspect ratio (16:9 or stretched, GameActivity.layoutSurface) and the presenter
        // fills the whole buffer.
        args.add("--present_letterbox=false");
        args.add("--present_safe_area_x=100");
        args.add("--present_safe_area_y=100");
        return args;
    }
}
