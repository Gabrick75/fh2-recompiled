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
    static final String PRESENT_MODE = "present_mode";        // "fifo" (vsync) or "mailbox"
    // Graphics
    static final String RENDER_SCALE = "render_scale";        // gpu_escala_render: percent of 1280x720
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
    static final String ONE_PASS = "one_pass";                // fh2_uma_faixa: the scene in one pass, not 3 tiles
    static final String ADAPTIVE_VBLANK = "adaptive_vblank";  // vblank_adaptativo
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
            case PRESENT_MODE: return "fifo";
            case RENDER_SCALE: return "100";
            case FPS: return "60";
            // Off: the GPU cost of the game's MSAA 4x nearly halves the frame rate on a phone (event menu on the
            // S23 Ultra: 6.8 fps 4x, 13.0 off), and the image is right without it (the MSAA cap leaves the game's
            // half-pitch 4x EDRAM clears alone, src/graphics/command_processor.cpp).
            case ANTI_ALIASING: return "off";
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
        boolean byDefault = PIN_THREADS.equals(key) || ONE_PASS.equals(key) || ADAPTIVE_VBLANK.equals(key);
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

    /** The typed render scale, 1 to 100 percent (100 when the stored text is not a number). */
    static int renderScalePercent(Context context) {
        try {
            return Math.max(1, Math.min(100, Integer.parseInt(get(context, RENDER_SCALE).trim())));
        } catch (NumberFormatException e) {
            return 100;
        }
    }

    /**
     * The scale the game really uses, in eighths of 1280x720 (8 = full): the render targets shrink in eighths so the
     * emulated EDRAM tiles stay whole pixels, and in quarters with MSAA 4x. Same rounding as
     * RenderTargetCache::GetConfigRenderEighths in the SDK.
     */
    static int renderScaleEighths(int percent, boolean msaa4x) {
        int eighths = Math.max(1, Math.min(8, (percent * 8 + 50) / 100));
        if (eighths < 8 && msaa4x) eighths = Math.max(eighths & ~1, 2);
        return eighths;
    }

    static boolean msaa4x(Context context) {
        return "msaa".equals(get(context, ANTI_ALIASING)) && "4".equals(get(context, MSAA_SAMPLES));
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
        args.add("--gpu_escala_render=" + renderScalePercent(context));
        args.add("--fh2_uma_faixa=" + flag(context, ONE_PASS));
        args.add("--vblank_adaptativo=" + flag(context, ADAPTIVE_VBLANK));
        args.add("--fh2_android_nucleos_grandes=" + flag(context, PIN_THREADS));
        // 'auto': the command processor alone on the fastest core (+16 % on the S23 Ultra), see android_rendimiento.cpp.
        args.add("--fh2_android_afinidade=" + (flag(context, PIN_THREADS) ? "auto" : "off"));  // never empty: the cvar parser aborts on it
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
        // FIFO presents with vsync. With MAILBOX the UI thread repaints the same guest frame as fast as it can: a
        // whole core and GPU time taken from the game (event menu on the S23 Ultra: 7.0 fps MAILBOX, 11.0 FIFO).
        args.add("--vulkan_allow_present_mode_mailbox=" + "mailbox".equals(get(context, PRESENT_MODE)));
        args.add("--vulkan_allow_present_mode_immediate=false");
        // The view gives the aspect ratio (16:9 or stretched, GameActivity.layoutSurface) and the presenter
        // fills the whole buffer.
        args.add("--present_letterbox=false");
        args.add("--present_safe_area_x=100");
        args.add("--present_safe_area_y=100");
        return args;
    }
}
