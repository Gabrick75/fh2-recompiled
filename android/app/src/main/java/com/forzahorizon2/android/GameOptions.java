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
 * Forza Horizon 2 runs on the emulated Xenos GPU by default, so the fixed arguments are the ones nfsmw-android found
 * the emulation needs on Android drivers. The native renderer (RENDERER) replaces it; it needs the translated shader
 * library, fh2_shaders.nfsp, next to the game files. For now it is built on the PC (tools/xenos/biblioteca.sh) and copied
 * with tools/enviar_biblioteca_android.sh; the plan is to build it on the phone from the player's own game files (see
 * docs/renderizador_nativo.md, "A biblioteca no celular").
 */
final class GameOptions {
    private static final String PREFS = "fh2_game";

    // Display
    static final String RESOLUTION = "resolution";          // "480p", "720p", "native": size of the presented buffer
    static final String SHOW_FPS = "show_fps";
    static final String SHOW_USAGE = "show_usage";            // GPU, CPU and RAM use under the FPS (UsageMonitor)
    static final String PRESENT_MODE = "present_mode";        // "fifo" (vsync) or "mailbox"
    // Graphics
    static final String RENDERER = "renderer";                // "emulated" (Xenos) or "native" (nfsmw_renderizador)
    static final String RENDER_SCALE = "render_scale";        // gpu_escala_render: percent of 1280x720
    static final String FPS = "fps";                          // the game's FPS limit, "0" = unlimited (fpsLimit)
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
    static final String RESOLVE_SHORTCUT = "resolve_shortcut";  // gpu_atalho_resolve
    static final String DRIVER = "driver";                    // imported driver name, "" = system; not loaded yet
    static final String TURBO = "turbo";                      // not applied yet
    // Sound
    static final String VOLUME = "volume";
    // Language
    static final String GAME_LANGUAGE = "game_language";  // a GameLanguage code: "en-US", "pt-BR"...
    static final String APP_LANGUAGE = "app_language";    // "pt" or "en" (AppLanguage)

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
            case RENDERER: return "emulated";
            case PRESENT_MODE: return "fifo";
            case RENDER_SCALE: return "100";
            case FPS: return "30";  // the Xbox 360's
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
            // English (US), what the console settings said before this option existed.
            case GAME_LANGUAGE: return "en-US";
            case APP_LANGUAGE: return "pt".equals(java.util.Locale.getDefault().getLanguage()) ? "pt" : "en";
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
        boolean byDefault = PIN_THREADS.equals(key) || ONE_PASS.equals(key) || ADAPTIVE_VBLANK.equals(key) ||
                RESOLVE_SHORTCUT.equals(key);
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

    /** The render scale of the native renderer: the typed percent, 50 to 100. */
    static int nativeRenderScalePercent(Context context) {
        return Math.max(50, renderScalePercent(context));
    }

    /** anisotropic_override's values (0, 2 = 2x, 3 = 4x, 5 = 16x) as nfsmw_nativo_anisotropico's (0, 2, 4, 16). */
    static String nativeAnisotropic(String value) {
        switch (value) {
            case "2": return "2";
            case "3": return "4";
            case "5": return "16";
            default: return "0";
        }
    }

    static boolean nativeRenderer(Context context) {
        return "native".equals(get(context, RENDERER));
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

    /**
     * The game's languages: the string tables on the disc (media/stringtables: EN, GB, FR, DE, ES, MX, IT, JP, br, PL,
     * RU, CHT). The game reads the language from the console settings (XGetLanguage, ExGetXConfigSetting ->
     * user_language) and the region from the locale (XGetLocale, XamGetLocaleEx -> user_country), and the pair picks
     * the variant: English of the US or the UK, Spanish of Spain or Mexico, the sat nav voice (sub_82E52468). Only
     * en-US, fr-FR, es-MX and pt-BR have their own voice-over (media/audio/vo).
     */
    static final class GameLanguage {
        final String code;
        final String name;          // in its own language, as a console lists them
        final String portuguese;    // what it is, in the app's two languages
        final String english;
        final int language;         // XLanguage (sdk/include/rex/system/xcontent.h)
        final int country;          // XOnlineCountry; xeXamGetLocaleFromCountry turns it into the locale
        final boolean voiceOver;

        GameLanguage(String code, String name, String portuguese, String english, int language, int country,
                     boolean voiceOver) {
            this.code = code;
            this.name = name;
            this.portuguese = portuguese;
            this.english = english;
            this.language = language;
            this.country = country;
            this.voiceOver = voiceOver;
        }
    }

    static final GameLanguage[] GAME_LANGUAGES = {
            new GameLanguage("en-US", "English (US)", "Inglês · Estados Unidos", "English · United States", 1, 103, true),
            new GameLanguage("en-GB", "English (UK)", "Inglês · Reino Unido", "English · United Kingdom", 1, 35, false),
            new GameLanguage("pt-BR", "Português (Brasil)", "Português · Brasil", "Portuguese · Brazil", 9, 13, true),
            new GameLanguage("es-ES", "Español (España)", "Espanhol · Espanha", "Spanish · Spain", 5, 31, false),
            new GameLanguage("es-MX", "Español (México)", "Espanhol · México", "Spanish · Mexico", 5, 71, true),
            new GameLanguage("fr-FR", "Français", "Francês", "French", 4, 34, true),
            new GameLanguage("de-DE", "Deutsch", "Alemão", "German", 3, 24, false),
            new GameLanguage("it-IT", "Italiano", "Italiano", "Italian", 6, 50, false),
            new GameLanguage("pl-PL", "Polski", "Polonês", "Polish", 11, 82, false),
            new GameLanguage("ru-RU", "Русский", "Russo", "Russian", 12, 88, false),
            new GameLanguage("ja-JP", "日本語", "Japonês", "Japanese", 2, 53, false),
            new GameLanguage("zh-TW", "繁體中文", "Chinês tradicional", "Traditional Chinese", 8, 101, false),
    };

    /**
     * The game's frame rate is paced by the emulated vblank, not by a timer of its own: in game it flips every 2nd
     * vblank (30 fps at the console's 60 Hz), in the start screens and loadings every one. So the limit is set by
     * the vblank rate, twice the chosen FPS: on the PC, 120 Hz took the loading screens from 55 to 104 fps and let
     * game frames under 30 ms through, and the game's clock is the real one (KeQueryPerformanceFrequency), so after
     * the same 20 s of throttle the car was at the same speed. Both renderers read the rate (VdQueryVideoMode in
     * graphics_system.cpp and in BucleVblank). Unlimited is 480 Hz, the SDK's top: 240 fps in game. The start
     * screens and loadings may go up to twice the limit.
     */
    static int vblankRate(Context context) {
        int fps;
        try {
            fps = Integer.parseInt(get(context, FPS));
        } catch (NumberFormatException e) {
            fps = 30;
        }
        return fps <= 0 ? 480 : Math.max(30, Math.min(240, fps)) * 2;
    }

    static GameLanguage gameLanguage(Context context) {
        String code = get(context, GAME_LANGUAGE);
        for (GameLanguage language : GAME_LANGUAGES) if (language.code.equals(code)) return language;
        return GAME_LANGUAGES[0];
    }

    static List<String> arguments(Context context) {
        List<String> args = new ArrayList<>();
        args.add("--audio_ganancia_pct=" + get(context, VOLUME));
        GpuDrivers.arguments(context, args);  // the chosen Vulkan driver, or the system's
        // The FPS limit is the guest vblank rate (fpsLimit).
        args.add("--video_mode_refresh_rate=" + vblankRate(context));
        GameLanguage language = gameLanguage(context);
        args.add("--user_language=" + language.language);
        args.add("--user_country=" + language.country);
        if ("native".equals(get(context, RENDERER))) {
            // NFSMW Android Evolved's renderer built for FH2 (app/src/nativo): the ring is drawn with translated
            // shaders, no EDRAM emulation. The emulated GPU's arguments below are then ignored; these are its own.
            args.add("--nfsmw_renderizador=nativo");
            // The scene and post-processing render targets drawn smaller (50 to 100 %, any step).
            args.add("--fh2_nativo_escala=" + nativeRenderScalePercent(context));
            // Its own FXAA on the output; it has no MSAA (the game's 4x surfaces are drawn with one sample).
            args.add("--nfsmw_antialiasing=" + ("fxaa".equals(get(context, ANTI_ALIASING)) ? "fxaa" : "apagado"));
            args.add("--nfsmw_nativo_anisotropico=" + nativeAnisotropic(get(context, ANISOTROPIC)));
        }
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
        args.add("--gpu_atalho_resolve=" + flag(context, RESOLVE_SHORTCUT));
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
