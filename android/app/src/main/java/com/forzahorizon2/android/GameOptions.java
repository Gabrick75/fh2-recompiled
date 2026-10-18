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
    static final String SCALING = "scaling";  // present_effect: bilinear, cas or fsr 1.0
    static final String SHARPNESS = "sharpness";  // present_cas_additional_sharpness * 100, 0 to 100
    static final String FSR_MODE = "fsr_mode";  // off or an FSR 1.0 preset: render scale + fsr effect
    static final String ANISOTROPIC = "anisotropic";          // anisotropic_override: 0, 2 (2x), 3 (4x), 5 (16x)
    static final String SHADOWS = "shadows";                  // not applied yet (needs game hooks)
    static final String SHADOW_DISTANCE = "shadow_distance";  // not applied yet
    static final String CAR_REFLECTIONS = "car_reflections";  // not applied yet
    static final String SHADOW_MAP_SCALE = "shadow_map_scale";  // nfsmw_nativo_sombras_escala: 0 (off) to 100 %
    // Performance
    static final String PIN_THREADS = "pin_threads";
    static final String JOGO_RAPIDO = "jogo_rapido";  // fh2_android_afinidade=jogo-rapido (off = auto)
    static final String PERFORMANCE_HINTS = "performance_hints";  // fh2_android_adpf (Android's ADPF)
    // The ADPF toggle is out of the menu: the hints are always on (the 2026-10-14 A/B on the S23 Ultra showed no
    // loss). Set to true to bring the row back (MainActivity.performancePage) and honor the saved choice again.
    static final boolean PERFORMANCE_HINTS_IN_MENU = false;
    static final String VERTEX_CACHE = "vertex_cache";  // fh2_nativo_cache_vertices (native), on by default
    static final String EXACT_TARGETS = "exact_targets";  // fh2_nativo_destinos_exactos (native), off by default
    // Diagnostics (About page)
    static final String GPU_MEASUREMENTS = "gpu_measurements";  // fh2_nativo_medir_gpu: GPU times in the logs
    static final String VERBOSE_LOG = "verbose_log";  // log_detalhado: every repeated message (stubs, textures)
    static final String ROOT_PROFILE = "root_profile";  // one simpleperf profile in the next game (root)
    static final String ONE_PASS = "one_pass";                // fh2_uma_faixa: the scene in one pass, not 3 tiles
    static final String ADAPTIVE_VBLANK = "adaptive_vblank";  // vblank_adaptativo
    static final String RESOLVE_SHORTCUT = "resolve_shortcut";  // gpu_atalho_resolve
    static final String ASYNC_SHADERS = "async_shaders";      // async_shader_compilation (emulated)
    static final String LAZY_COMPOSITE = "lazy_composite";    // nfsmw_nativo_compuesta_perezosa (native)
    static final String DRIVER = "driver";                    // imported driver name, "" = system; not loaded yet
    static final String TURBO = "turbo";                      // not applied yet
    // Sound
    static final String VOLUME = "volume";
    // audio_xma_en_trabajador: on = 1 (the XMA Decoder thread decodes everything), off = 2 (the game's audio
    // thread decodes, as before f47050e). Off by default: on several phones mode 1 made the sound stutter.
    static final String AUDIO_WORKER = "audio_worker";
    static final String VIBRATION_PHONE = "vibration_phone";
    static final String VIBRATION_MODE = "vibration_mode";  // "phone", "gamepad", "both"
    // Language
    static final String GAME_LANGUAGE = "game_language";  // a GameLanguage code: "en-US", "pt-BR"...
    static final String APP_LANGUAGE = "app_language";    // "pt" or "en" (AppLanguage)
    // Game files
    static final String GAME_ISO = "game_iso";            // path of the disc image read in place; "" = the folder

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
            case SHARPNESS: return "0";
            case FSR_MODE: return "off";
            case ANISOTROPIC: return "3";
            case SHADOWS: return "1";
            case SHADOW_DISTANCE: return "far";
            case CAR_REFLECTIONS: return "high";
            case SHADOW_MAP_SCALE: return "100";  // the Xbox 360's 1600x1600
            case DRIVER: return "";
            case VOLUME: return "100";
            case VIBRATION_PHONE: return "true";
            case VIBRATION_MODE: return "both";
            // English (US), what the console settings said before this option existed.
            case GAME_LANGUAGE: return "en-US";
            case GAME_ISO: return "";
            case APP_LANGUAGE: return "pt".equals(java.util.Locale.getDefault().getLanguage()) ? "pt" : "en";
            default: throw new IllegalArgumentException(key);
        }
    }

    static String get(Context context, String key) {
        String value = prefs(context).getString(key, defaultOf(key));
        if (ANTI_ALIASING.equals(key) && ("msaa2".equals(value) || "msaa4".equals(value))) {
            // Before the native renderer used the emulated one's form, it stored "msaa2"/"msaa4": both renderers
            // now keep "msaa" here and the sample count in MSAA_SAMPLES.
            prefs(context).edit().putString(ANTI_ALIASING, "msaa")
                    .putString(MSAA_SAMPLES, value.substring(4)).apply();
            value = "msaa";
        }
        return value;
    }

    static void set(Context context, String key, String value) {
        prefs(context).edit().putString(key, value).apply();
    }

    static boolean flag(Context context, String key) {
        boolean byDefault = PIN_THREADS.equals(key) || PERFORMANCE_HINTS.equals(key) || ONE_PASS.equals(key) ||
                ADAPTIVE_VBLANK.equals(key) || RESOLVE_SHORTCUT.equals(key) || VERTEX_CACHE.equals(key) ||
                VIBRATION_PHONE.equals(key);
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

    /** The render scale of the native renderer: the typed percent, 12 (160x90) to 100. */
    static int nativeRenderScalePercent(Context context) {
        return Math.max(12, renderScalePercent(context));
    }

    /**
     * FSR 1.0 mode as a render-scale percent of 1280x720, or 0 when off. Only while the scaling effect
     * itself is fsr: otherwise a stored mode must never override the typed scale (stale preset).
     */
    static int fsrModeScalePercent(Context context) {
        if (!"fsr".equals(get(context, SCALING))) return 0;
        switch (get(context, FSR_MODE)) {
            case "ultra_quality": return 77;
            case "quality": return 67;
            case "balanced": return 59;
            case "performance": return 50;
            case "ultra_performance": return 33;
            default: return 0;
        }
    }

    /** The eighths/percent above, with an FSR preset winning over the typed scale when one is on. */
    static int effectiveRenderScalePercent(Context context) {
        int preset = fsrModeScalePercent(context);
        return preset > 0 ? preset : renderScalePercent(context);
    }

    /** The native scale above, with an FSR preset winning over the typed scale when one is on. */
    static int effectiveNativeRenderScalePercent(Context context) {
        int preset = fsrModeScalePercent(context);
        return preset > 0 ? Math.max(12, preset) : nativeRenderScalePercent(context);
    }

    /** The native renderer's shadow map size, 0 (no shadows) to 100 percent of the game's 1600x1600. */
    static int shadowMapScalePercent(Context context) {
        try {
            return Math.max(0, Math.min(100, Integer.parseInt(get(context, SHADOW_MAP_SCALE).trim())));
        } catch (NumberFormatException e) {
            return 100;
        }
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
     * graphics_system.cpp and in BucleVblank). The start screens and loadings may go up to twice the limit.
     * Unlimited is 240 Hz, the same as 120 fps: at 480 Hz the game stopped drawing its 3D world (black race on the
     * Moto G9 Play, ~200 draws per frame against ~1,700 at 60 Hz), and no phone reaches 120 fps in a race anyway.
     */
    static int vblankRate(Context context) {
        int fps;
        try {
            fps = Integer.parseInt(get(context, FPS));
        } catch (NumberFormatException e) {
            fps = 30;
        }
        return fps <= 0 ? 240 : Math.max(30, Math.min(120, fps)) * 2;
    }

    static GameLanguage gameLanguage(Context context) {
        String code = get(context, GAME_LANGUAGE);
        for (GameLanguage language : GAME_LANGUAGES) if (language.code.equals(code)) return language;
        return GAME_LANGUAGES[0];
    }

    static List<String> arguments(Context context) {
        List<String> args = new ArrayList<>();
        args.add("--audio_ganancia_pct=" + get(context, VOLUME));
        // Repeated messages (kernel stubs, resolved textures): the first 20 of each unless the player asks for all.
        args.add("--log_detalhado=" + flag(context, VERBOSE_LOG));
        args.add("--audio_xma_en_trabajador=" + (flag(context, AUDIO_WORKER) ? 1 : 2));
        // Phone vibration (android_vibration.cpp): read once at start, never through the JVM per frame.
        args.add("--fh2_android_vibracao=" + flag(context, VIBRATION_PHONE));
        String vibrationMode = get(context, VIBRATION_MODE);
        if (!"phone".equals(vibrationMode) && !"gamepad".equals(vibrationMode)) vibrationMode = "both";
        args.add("--fh2_android_vibracao_modo=" + vibrationMode);  // never empty: the cvar parser aborts on it
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
            // The scene and post-processing render targets drawn smaller (12 to 100 %, any step; 12 is 160x90).
            args.add("--fh2_nativo_escala=" + effectiveNativeRenderScalePercent(context));
            // Its own FXAA on the output; MSAA 2x/4x on the 3D scene (fh2_nativo_msaa, off by default).
            args.add("--nfsmw_antialiasing=" + ("fxaa".equals(get(context, ANTI_ALIASING)) ? "fxaa" : "apagado"));
            // The same choice as the emulated renderer's: "msaa" and the sample count (2 or 4).
            args.add("--fh2_nativo_msaa=" + (!"msaa".equals(get(context, ANTI_ALIASING)) ? 0
                    : "2".equals(get(context, MSAA_SAMPLES)) ? 2 : 4));
            args.add("--nfsmw_nativo_anisotropico=" + nativeAnisotropic(get(context, ANISOTROPIC)));
            args.add("--nfsmw_nativo_sombras_escala=" + shadowMapScalePercent(context));
            args.add("--nfsmw_nativo_compuesta_perezosa=" + flag(context, LAZY_COMPOSITE));
            // Off by default: each GPU timestamp makes a phone's GPU wait for the work in flight.
            args.add("--fh2_nativo_medir_gpu=" + flag(context, GPU_MEASUREMENTS));
            args.add("--fh2_nativo_cache_vertices=" + flag(context, VERTEX_CACHE));
            args.add("--fh2_nativo_destinos_exactos=" + flag(context, EXACT_TARGETS));
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
        args.add("--gpu_escala_render=" + effectiveRenderScalePercent(context));
        args.add("--fh2_uma_faixa=" + flag(context, ONE_PASS));
        args.add("--vblank_adaptativo=" + flag(context, ADAPTIVE_VBLANK));
        args.add("--gpu_atalho_resolve=" + flag(context, RESOLVE_SHORTCUT));
        args.add("--fh2_android_nucleos_grandes=" + flag(context, PIN_THREADS));
        // 'auto': the command processor alone on the fastest core (+16 % on the S23 Ultra), see android_rendimiento.cpp.
        // 'jogo-rapido': the busiest game thread alone on the fastest core instead (for heavy scenery, where the
        // game thread saturates first); off unless PIN_THREADS and JOGO_RAPIDO are both on.
        args.add("--fh2_android_afinidade=" + (!flag(context, PIN_THREADS) ? "off"
                : flag(context, JOGO_RAPIDO) ? "jogo-rapido" : "auto"));  // never empty: the cvar parser aborts on it
        args.add("--fh2_android_adpf=" + (!PERFORMANCE_HINTS_IN_MENU || flag(context, PERFORMANCE_HINTS)));
        // Emulated Xenos GPU on Android drivers (from nfsmw-android's "xenos" mode): ordinary descriptor sets,
        // host framebuffers, no geometry shader or non-solid fill required, shaders compiled before drawing.
        args.add("--gpu_plugin=xenos");
        args.add("--vulkan_native_shader_features=false");
        args.add("--render_target_path_vulkan=fbo");
        args.add("--vulkan_require_geometry_shader=false");
        args.add("--vulkan_require_fill_mode_non_solid=false");
        // Off: a draw waits for its pipeline. On: it is skipped until the pipeline is ready (less stutter, brief gaps).
        args.add("--async_shader_compilation=" + flag(context, ASYNC_SHADERS));
        // Forza Horizon 2 leaves vertex fetch constants with type 0 in some draws; without this the emulated GPU
        // drops them (same as tools/rodar_pc.sh on the PC).
        args.add("--gpu_allow_invalid_fetch_constants=true");
        // FIFO presents with vsync. With MAILBOX the UI thread repaints the same guest frame as fast as it can: a
        // whole core and GPU time taken from the game (event menu on the S23 Ultra: 7.0 fps MAILBOX, 11.0 FIFO).
        args.add("--vulkan_allow_present_mode_mailbox=" + "mailbox".equals(get(context, PRESENT_MODE)));
        args.add("--vulkan_allow_present_mode_immediate=false");
        // The view gives the aspect ratio (16:9 or stretched, GameActivity.layoutSurface) and the presenter
        // fills the whole buffer. The scaling effect (bilinear, CAS sharpening or FSR 1.0) needs
        // REX_HAS_FIDELITYFX_SDK on Android (sdk/src/ui/CMakeLists.txt).
        String scaling = get(context, SCALING);
        // An FSR mode on wins over the scaling choice: it is a scale preset with the fsr effect.
        if (fsrModeScalePercent(context) > 0) scaling = "fsr";
        if (!"cas".equals(scaling) && !"fsr".equals(scaling)) scaling = "bilinear";
        args.add("--present_effect=" + scaling);
        // Extra CAS/RCAS sharpness on top of the effect's own (0 keeps the SDK default).
        int sharpness;
        try {
            sharpness = Math.max(0, Math.min(100, Integer.parseInt(get(context, SHARPNESS).trim())));
        } catch (NumberFormatException e) {
            sharpness = 0;
        }
        args.add("--present_cas_additional_sharpness=" + (sharpness / 100.0));
        args.add("--present_letterbox=false");
        args.add("--present_safe_area_x=100");
        args.add("--present_safe_area_y=100");
        return args;
    }
}
