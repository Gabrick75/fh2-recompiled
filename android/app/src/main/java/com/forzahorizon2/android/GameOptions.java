package com.forzahorizon2.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.hardware.display.DisplayManager;
import android.view.Display;

import java.util.ArrayList;
import java.util.List;

/**
 * The launcher's settings (the menu follows NFSMW-Recompiled-Mobile's). Most of them become game cvars that
 * GameActivity passes on the command line, which wins over fh2.toml; the display ones are applied by GameActivity
 * itself. Some are only stored for now: their option row says so (see MainActivity.notYet).
 *
 * Forza Horizon 2 runs on the native renderer by default (RENDERER); the emulated Xenos GPU's fixed arguments are the
 * ones nfsmw-android found the emulation needs on Android drivers. The native renderer needs the translated shader
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
    static final String PRESENT_THREAD = "present_thread";    // present_hilo_propio, off by default (unstable)
    // present_hdr: HDR10 output, offered only where the display reports HDR10 (hdrCapabilities)
    static final String HDR = "hdr";
    static final String HDR_PAPER_WHITE = "hdr_paper_white";  // present_hdr_paper_white_nits: the HUD's and menus' white
    static final String[] HDR_PAPER_WHITES = {"100", "150", "200", "250", "300", "400"};
    // present_hdr_highlights: how far the scene's white goes from the paper white to the display's peak (the HUD stays)
    static final String HDR_HIGHLIGHTS = "hdr_highlights";
    static final String[] HDR_HIGHLIGHT_LEVELS = {"0", "0.25", "0.5", "1"};  // Pinyon Shift's OFF, LOW, MEDIUM, HIGH
    /*
     * The peak the HDR10 output declares and expands to: "auto" (the display's reported peak, at least
     * HDR_PEAK_AUTO_MIN), "reported" or a number of nits. The S23 Ultra reports 450 nits but its panel shows more: on
     * 2026-10-25 a declared 1000 made the highlights visibly stronger.
     */
    static final String HDR_PEAK = "hdr_peak";
    static final String[] HDR_PEAKS = {"auto", "reported", "1500"};
    static final float HDR_PEAK_AUTO_MIN = 1000f;
    // Relative HDR: the headroom the game's surface asks for, as peak / SDR white. Android gives at most what the
    // display can at its current brightness (the presenter follows that, GameActivity.startRelativeHdr); 8 lets a
    // panel like the S23 Ultra's (about 1750 nits at its HDR peak) reach it from an ordinary indoor brightness.
    static final float HDR_DESIRED_HEADROOM = 8f;
    // Graphics
    static final String RENDERER = "renderer";                // "emulated" (Xenos) or "native" (nfsmw_renderizador)
    static final String RENDER_SCALE = "render_scale";        // gpu_escala_render: percent of 1280x720
    static final String FPS = "fps";                          // the game's FPS limit, "0" = unlimited (fpsLimit)
    static final String ANTI_ALIASING = "anti_aliasing";      // "off", "msaa", "fxaa"
    static final String MSAA_SAMPLES = "msaa_samples";        // "2", "4"
    static final String FXAA_QUALITY = "fxaa_quality";        // "normal", "high"
    static final String SCALING = "scaling";  // off (the typed scale, nearest: no filter) or present_effect: bilinear, cas or fsr 1.0
    static final String SHARPNESS = "sharpness";  // present_cas_additional_sharpness * 100, 0 to 100
    static final String FSR_MODE = "fsr_mode";  // an FSR 1.0 preset: render scale + fsr effect
    static final String ANISOTROPIC = "anisotropic";          // anisotropic_override: 0, 2 (2x), 3 (4x), 5 (16x)
    static final String SHADOWS = "shadows";                  // not applied yet (needs game hooks)
    static final String SHADOW_DISTANCE = "shadow_distance";  // not applied yet
    static final String CAR_REFLECTIONS = "car_reflections";  // not applied yet
    static final String SHADOW_MAP_SCALE = "shadow_map_scale";  // nfsmw_nativo_sombras_escala: 0 (off) to 100 %
    // World detail (app/src/fh2_mundo.cpp): factors of the Xbox 360's, WORLD_FACTORS, "1" = as it. Both renderers but
    // grass, which the native renderer draws (0 = none).
    static final String WORLD_GRASS = "world_grass";              // fh2_mundo_grama
    static final String WORLD_OBJECTS = "world_objects";          // fh2_mundo_objetos
    static final String WORLD_REFLECTIONS = "world_reflections";  // fh2_mundo_reflexos
    static final String WORLD_SHADOWS = "world_shadows";          // fh2_mundo_sombras
    static final String WORLD_CROWD = "world_crowd";              // fh2_mundo_multidao
    // fh2_nativo_msaa_a2c (native, with MSAA): foliage with real alpha to coverage, soft but grainy ("flour") on
    // phones; off by default (the alpha test)
    static final String FOLIAGE_COVERAGE = "foliage_coverage";
    static final float[] WORLD_FACTORS = {0.25f, 0.5f, 0.75f, 1f, 1.5f, 2f, 3f, 4f};
    // All but objects also go down to off (0): grass and crowd skip their draws and shadows have no shadow map in the
    // native renderer, reflections lose the live cubemap in both. Objects off would take the world away.
    static final float[] WORLD_FACTORS_OFF = {0f, 0.25f, 0.5f, 0.75f, 1f, 1.5f, 2f, 3f, 4f};

    /** The positions of a world detail slider: WORLD_FACTORS for objects, WORLD_FACTORS_OFF for the others. */
    static float[] worldFactors(String key) {
        return WORLD_OBJECTS.equals(key) ? WORLD_FACTORS : WORLD_FACTORS_OFF;
    }
    // Performance
    static final String PIN_THREADS = "pin_threads";
    static final String JOGO_RAPIDO = "jogo_rapido";  // fh2_android_afinidade=jogo-rapido (off = auto)
    // fh2_android_adpf (Android's ADPF). Not "performance_hints": the 2026-10-14 A/B builds saved that key off on some
    // phones, and the toggle starts on for everyone.
    static final String PERFORMANCE_HINTS = "adpf_hints";
    // The ADPF toggle is in Performance > Processor, on by default (the 2026-10-14 A/B on the S23 Ultra showed no
    // loss). Set to false to hide the row again and keep the hints always on.
    static final boolean PERFORMANCE_HINTS_IN_MENU = true;
    static final String VERTEX_CACHE = "vertex_cache";  // fh2_nativo_cache_vertices (native), on by default
    // fh2_nativo_cache_indices (native), on by default. Not "indices_cache": test builds of 2026-10-14 used that key
    // and some phones saved it off to dodge a since-fixed bug.
    static final String INDEX_CACHE = "index_cache";
    static final String VERTEX_NO_WAIT = "vertex_no_wait";  // fh2_nativo_vertices_sin_espera (native), on by default
    static final String EXACT_TARGETS = "exact_targets";  // fh2_nativo_destinos_exactos (native), on by default
    // fh2_nativo_mips_reflejo_juntos (native), on by default (2026-10-26: 48 passes fewer, matches the emulated image)
    static final String REFLECTION_MIPS = "reflection_mips";
    static final String TIGHT_AREA = "tight_area";  // fh2_nativo_area_justa (native), on by default
    // fh2_reflexo_faces: the car reflection cube redrawn 2 faces a frame instead of 6 (both renderers), on by default
    // (2026-10-26, S23 A/B in one session: +5 % fps, no visible difference)
    static final String REFLECTION_STAGGER = "reflection_stagger";
    // Diagnostics (About page)
    static final String GPU_MEASUREMENTS = "gpu_measurements";  // fh2_nativo_medir_gpu: GPU times in the logs
    static final String VERBOSE_LOG = "verbose_log";  // log_detalhado: every repeated message (stubs, textures)
    static final String ROOT_PROFILE = "root_profile";  // one simpleperf profile in the next game (root)
    static final String ONE_PASS = "one_pass";                // fh2_uma_faixa: the scene in one pass, not 3 tiles
    static final String ADAPTIVE_VBLANK = "adaptive_vblank";  // vblank_adaptativo
    static final String RESOLVE_SHORTCUT = "resolve_shortcut";  // gpu_atalho_resolve
    static final String ASYNC_SHADERS = "async_shaders";      // async_shader_compilation (emulated)
    static final String LAZY_COMPOSITE = "lazy_composite";    // nfsmw_nativo_compuesta_perezosa (native)
    static final String STEP_PACING = "step_pacing";          // fh2_ritmo_passos: one game step per frame, on
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
    static final String APP_LANGUAGE = "app_language";    // "" (the system's), "pt", "en" or "es" (AppLanguage)
    // Game files
    static final String GAME_ISO = "game_iso";            // path of the disc image read in place; "" = the folder
    // Home screen
    // Extra game options for tests: "--name=value" words, appended after the launcher's own (so they win).
    static final String EXTRA_ARGS = "extra_args";
    static final String GAMERTAG = "gamertag";  // user_gamertag: the name the game shows (cleanGamertag)
    static final String HOME_VIDEO_SOUND = "home_video_sound";  // the home screen video with sound, off by default

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
            case RENDERER: return "native";
            case PRESENT_MODE: return "fifo";
            case HDR_PAPER_WHITE: return "200";  // Pinyon Shift's default
            case HDR_HIGHLIGHTS: return "0.5";
            case HDR_PEAK: return "auto";
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
            case FSR_MODE: return "mega_quality";
            case ANISOTROPIC: return "0";
            case SHADOWS: return "1";
            case SHADOW_DISTANCE: return "far";
            case CAR_REFLECTIONS: return "high";
            case SHADOW_MAP_SCALE: return "100";  // the Xbox 360's 1600x1600
            case WORLD_GRASS:
            case WORLD_OBJECTS:
            case WORLD_REFLECTIONS:
            case WORLD_SHADOWS:
            case WORLD_CROWD: return "1";  // the Xbox 360's
            case DRIVER: return "";
            case VOLUME: return "100";
            case VIBRATION_PHONE: return "true";
            case VIBRATION_MODE: return "both";
            case GAME_LANGUAGE: return systemGameLanguage();
            case GAME_ISO: return "";
            case GAMERTAG: return "Player";
            case APP_LANGUAGE: return "";  // the system's: Android picks from the phone's languages
            case EXTRA_ARGS: return "";
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

    static final int GAMERTAG_MAX = 15;

    /**
     * A name as an Xbox 360 gamertag: letters, digits and single spaces, at most 15, so it fits wherever the game
     * shows one. Empty when nothing is left.
     */
    static String cleanGamertag(String name) {
        StringBuilder clean = new StringBuilder();
        for (int i = 0; i < name.length() && clean.length() < GAMERTAG_MAX; i++) {
            char c = name.charAt(i);
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) clean.append(c);
            else if (c == ' ' && clean.length() > 0 && clean.charAt(clean.length() - 1) != ' ') clean.append(c);
        }
        return clean.toString().trim();
    }

    static String gamertag(Context context) {
        String name = cleanGamertag(get(context, GAMERTAG));
        return name.isEmpty() ? defaultOf(GAMERTAG) : name;  // never empty: the cvar parser aborts on it
    }

    static boolean flag(Context context, String key) {
        boolean byDefault = PIN_THREADS.equals(key) || PERFORMANCE_HINTS.equals(key) || ONE_PASS.equals(key) ||
                ADAPTIVE_VBLANK.equals(key) || RESOLVE_SHORTCUT.equals(key) || VERTEX_NO_WAIT.equals(key) ||
                EXACT_TARGETS.equals(key) || VIBRATION_PHONE.equals(key) || TIGHT_AREA.equals(key) ||
                REFLECTION_MIPS.equals(key) || STEP_PACING.equals(key) || REFLECTION_STAGGER.equals(key) ||
                ((VERTEX_CACHE.equals(key) || INDEX_CACHE.equals(key)) && !crossFrameCachesOffByDefault(context));
        return prefs(context).getBoolean(key, byDefault);
    }

    /**
     * The native renderer's caches across frames (vertices, indices and FH2's bone and instance pools) trust its
     * tracking of guest writes. On a Mali-G1 Ultra each one served stale data now and then: the car jumped for a
     * frame, and the renderer's own checks reported differences. With the three off it held 30 fps with one skipped
     * frame in a race, so on Mali they start off (the vertex and index ones stay as toggles).
     */
    static boolean crossFrameCachesOffByDefault(Context context) {
        return GpuDrivers.isMali(context);
    }

    // What "Restore defaults" (the Graphics and Performance pages) takes back: every option of those two pages, the
    // hidden ones too, but the imported GPU driver.
    private static final String[] RESTORABLE = {RENDERER, RENDER_SCALE, FPS, ANTI_ALIASING, MSAA_SAMPLES, FXAA_QUALITY,
            SCALING, SHARPNESS, FSR_MODE, ANISOTROPIC, SHADOWS, SHADOW_DISTANCE, CAR_REFLECTIONS, SHADOW_MAP_SCALE,
            WORLD_GRASS, WORLD_OBJECTS, WORLD_REFLECTIONS, WORLD_SHADOWS, WORLD_CROWD, FOLIAGE_COVERAGE, ONE_PASS, RESOLVE_SHORTCUT, ASYNC_SHADERS, LAZY_COMPOSITE, STEP_PACING, PIN_THREADS, JOGO_RAPIDO, PRESENT_THREAD, PERFORMANCE_HINTS,
            VERTEX_CACHE, INDEX_CACHE, VERTEX_NO_WAIT, EXACT_TARGETS, REFLECTION_MIPS, TIGHT_AREA, REFLECTION_STAGGER, TURBO};

    /** The Graphics and Performance options back to their defaults (defaultOf, flag): the stored values go. */
    static void restoreDefaults(Context context) {
        SharedPreferences.Editor editor = prefs(context).edit();
        for (String key : RESTORABLE) editor.remove(key);
        editor.apply();
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

    // The steering arrows also live with the touch controls; the launcher sets them before the game reads them.
    static boolean steeringArrows(Context context) {
        return context.getSharedPreferences("fh2_controls", Context.MODE_PRIVATE).getBoolean("steering_arrows", false);
    }

    static void steeringArrows(Context context, boolean value) {
        context.getSharedPreferences("fh2_controls", Context.MODE_PRIVATE).edit()
                .putBoolean("steering_arrows", value).apply();
    }

    static float arrowLock(Context context) {
        return context.getSharedPreferences("fh2_controls", Context.MODE_PRIVATE).getFloat("arrow_lock", 1f);
    }

    static void arrowLock(Context context, float value) {
        context.getSharedPreferences("fh2_controls", Context.MODE_PRIVATE).edit().putFloat("arrow_lock", value).apply();
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
     * Render-scale percent of 1280x720 forced by the scaling choice, or 0 to keep the typed scale. Only FSR 1.0
     * forces one, its mode. Off, bilinear and CAS keep the typed scale, so a stored FSR mode never overrides it
     * (stale preset).
     */
    static int fsrModeScalePercent(Context context) {
        if (!"fsr".equals(get(context, SCALING))) return 0;
        switch (get(context, FSR_MODE)) {
            case "mega_quality": return 100;
            case "ultra_quality": return 77;
            case "quality": return 67;
            case "balanced": return 59;
            case "performance": return 50;
            case "ultra_performance": return 33;
            default: return 100;  // mega_quality, and the old "off"
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

    /** A world detail factor (WORLD_GRASS and the others), one of worldFactors(key); 1 when the stored one is not. */
    static float worldFactor(Context context, String key) {
        try {
            float value = Float.parseFloat(get(context, key).trim());
            for (float factor : worldFactors(key)) {
                if (Math.abs(factor - value) < 0.001f) return factor;
            }
        } catch (NumberFormatException e) {
            // the default below
        }
        return 1f;
    }

    static void worldFactor(Context context, String key, float value) {
        set(context, key, String.valueOf(value));
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

    /**
     * The default display's HDR10 levels as Android reports them, {peak, frame average, minimum} in nits (0 when
     * unknown), or null when it does not report HDR10. The presenter takes them as the HDR10 metadata, so the panel
     * shows the levels as they are instead of tone-mapping them from an assumed master.
     */
    static float[] hdrCapabilities(Context context) {
        DisplayManager displays = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        Display display = displays != null ? displays.getDisplay(Display.DEFAULT_DISPLAY) : null;
        Display.HdrCapabilities caps = display != null ? display.getHdrCapabilities() : null;
        if (caps == null) return null;
        boolean hdr10 = false;
        for (int type : caps.getSupportedHdrTypes()) {
            hdr10 |= type == Display.HdrCapabilities.HDR_TYPE_HDR10;
        }
        if (!hdr10) return null;
        return new float[] {Math.max(0f, caps.getDesiredMaxLuminance()),
                Math.max(0f, caps.getDesiredMaxAverageLuminance()), Math.max(0f, caps.getDesiredMinLuminance())};
    }

    /**
     * Relative HDR (Android 15 and later, where the display reports its HDR/SDR ratio): the game's white is the
     * screen's own at the brightness the player chose, and the highlights reach the display's current peak, so the
     * white brightness option is not needed. Elsewhere HDR10 with that option.
     */
    @android.annotation.TargetApi(35)
    static boolean hdrRelative(Context context) {
        if (android.os.Build.VERSION.SDK_INT < 35 || hdrCapabilities(context) == null) return false;
        DisplayManager displays = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        Display display = displays != null ? displays.getDisplay(Display.DEFAULT_DISPLAY) : null;
        return display != null && display.isHdrSdrRatioAvailable();
    }

    /** The default display's current HDR/SDR ratio (1 when unknown): where the highlights start, before it changes. */
    @android.annotation.TargetApi(34)
    static float hdrSdrRatio(Context context) {
        if (android.os.Build.VERSION.SDK_INT < 34) return 1f;
        DisplayManager displays = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        Display display = displays != null ? displays.getDisplay(Display.DEFAULT_DISPLAY) : null;
        return display != null && display.isHdrSdrRatioAvailable() ? Math.max(1f, display.getHdrSdrRatio()) : 1f;
    }

    /** The paper white in nits, one of HDR_PAPER_WHITES (the default when the stored one is not). */
    static String hdrPaperWhite(Context context) {
        String value = get(context, HDR_PAPER_WHITE);
        for (String nits : HDR_PAPER_WHITES) {
            if (nits.equals(value)) return nits;
        }
        return defaultOf(HDR_PAPER_WHITE);
    }

    /** The peak to declare in nits (HDR_PEAK) from the display's reported one. */
    static float hdrPeakNits(Context context, float reported) {
        switch (get(context, HDR_PEAK)) {
            case "reported": return reported;
            case "1500": return 1500f;
            default: return Math.max(reported, HDR_PEAK_AUTO_MIN);  // auto
        }
    }

    /** The highlight level, one of HDR_HIGHLIGHT_LEVELS (the default when the stored one is not). */
    static String hdrHighlights(Context context) {
        String value = get(context, HDR_HIGHLIGHTS);
        for (String level : HDR_HIGHLIGHT_LEVELS) {
            if (level.equals(value)) return level;
        }
        return defaultOf(HDR_HIGHLIGHTS);
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
        final int description;      // what it is, in the app's language (R.string)
        final int language;         // XLanguage (sdk/include/rex/system/xcontent.h)
        final int country;          // XOnlineCountry; xeXamGetLocaleFromCountry turns it into the locale
        final boolean voiceOver;

        GameLanguage(String code, String name, int description, int language, int country, boolean voiceOver) {
            this.code = code;
            this.name = name;
            this.description = description;
            this.language = language;
            this.country = country;
            this.voiceOver = voiceOver;
        }
    }

    static final GameLanguage[] GAME_LANGUAGES = {
            new GameLanguage("en-US", "English (US)", R.string.game_language_en_us, 1, 103, true),
            new GameLanguage("en-GB", "English (UK)", R.string.game_language_en_gb, 1, 35, false),
            new GameLanguage("pt-BR", "Português (Brasil)", R.string.game_language_pt_br, 9, 13, true),
            new GameLanguage("es-ES", "Español (España)", R.string.game_language_es_es, 5, 31, false),
            new GameLanguage("es-MX", "Español (México)", R.string.game_language_es_mx, 5, 71, true),
            new GameLanguage("fr-FR", "Français", R.string.game_language_fr_fr, 4, 34, true),
            new GameLanguage("de-DE", "Deutsch", R.string.game_language_de_de, 3, 24, false),
            new GameLanguage("it-IT", "Italiano", R.string.game_language_it_it, 6, 50, false),
            new GameLanguage("pl-PL", "Polski", R.string.game_language_pl_pl, 11, 82, false),
            new GameLanguage("ru-RU", "Русский", R.string.game_language_ru_ru, 12, 88, false),
            new GameLanguage("ja-JP", "日本語", R.string.game_language_ja_jp, 2, 53, false),
            new GameLanguage("zh-TW", "繁體中文", R.string.game_language_zh_tw, 8, 101, false),
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
    /**
     * The words of the "Extra game options" field that look like "--name=value" with a value: anything else is
     * dropped, because the cvar parser aborts on an empty value.
     */
    static List<String> extraArguments(String text) {
        List<String> words = new ArrayList<>();
        for (String word : text.trim().split("\\s+")) {
            int equals = word.indexOf('=');
            if (word.startsWith("--") && equals > 2 && equals < word.length() - 1) {
                words.add(word);
            }
        }
        return words;
    }

    static int vblankRate(Context context) {
        int fps;
        try {
            fps = Integer.parseInt(get(context, FPS));
        } catch (NumberFormatException e) {
            fps = 30;
        }
        return fps <= 0 ? 240 : Math.max(30, Math.min(120, fps)) * 2;
    }

    /**
     * The game's language until the player picks one: the closest to the phone's languages (not the app's), in
     * their order. The exact one first (en-GB, es-ES), else the first of the same language (es: Mexico's, which is
     * voiced); English (US) when none matches.
     */
    static String systemGameLanguage() {
        android.os.LocaleList locales = android.content.res.Resources.getSystem().getConfiguration().getLocales();
        for (int i = 0; i < locales.size(); i++) {
            java.util.Locale locale = locales.get(i);
            String language = locale.getLanguage();
            String tag = language + "-" + locale.getCountry();
            for (GameLanguage game : GAME_LANGUAGES) if (game.code.equalsIgnoreCase(tag)) return game.code;
            if ("es".equals(language)) return "es-MX";
            for (GameLanguage game : GAME_LANGUAGES) if (game.code.startsWith(language + "-")) return game.code;
        }
        return "en-US";
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
        // The keyboard and the mouse as a controller (the SDK's MnK driver, sdk/src/input/mnk), always on, the way a
        // gamepad is: the driver is one more device on player 1, at rest until a key is pressed or the mouse moves,
        // and it takes the mouse only once a real one is used. The racing layout fills the keys the player did not
        // change in game (F4).
        args.add("--mnk_mode=true");
        args.add("--mnk_preset=race");
        args.add("--mnk_mouse=true");
        GpuDrivers.arguments(context, args);  // the chosen Vulkan driver, or the system's
        // The FPS limit is the guest vblank rate (fpsLimit).
        args.add("--video_mode_refresh_rate=" + vblankRate(context));
        args.add("--fh2_ritmo_passos=" + flag(context, STEP_PACING));
        // The game's EnvMapFrequencyScale capped (fh2_desempenho.cpp): the whole cube over 3 frames instead of every frame.
        args.add("--fh2_reflexo_faces=" + (flag(context, REFLECTION_STAGGER) ? 2 : 6));
        // World detail: the game's TrackSettings.xml rewritten at start (fh2_mundo.cpp), both renderers; grass is the
        // native renderer's (the vegetation's fade distances in its draws).
        args.add("--fh2_mundo_grama=" + worldFactor(context, WORLD_GRASS));
        args.add("--fh2_mundo_objetos=" + worldFactor(context, WORLD_OBJECTS));
        args.add("--fh2_mundo_reflexos=" + worldFactor(context, WORLD_REFLECTIONS));
        args.add("--fh2_mundo_sombras=" + worldFactor(context, WORLD_SHADOWS));
        args.add("--fh2_mundo_multidao=" + worldFactor(context, WORLD_CROWD));
        GameLanguage language = gameLanguage(context);
        args.add("--user_language=" + language.language);
        args.add("--user_country=" + language.country);
        args.add("--user_gamertag=" + gamertag(context));
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
            // World detail shadows off: no shadow map at all, whatever the shadow quality.
            args.add("--nfsmw_nativo_sombras_escala="
                    + (worldFactor(context, WORLD_SHADOWS) == 0f ? 0 : shadowMapScalePercent(context)));
            // Trees, bushes and fences (the alpha-tested draws) cast shadows from 1x up, as on the Xbox 360; below 1x
            // they are left out of the shadow map, as NFSMW does for speed (scene pass ~3.9 to ~2 ms of GPU on the PC).
            args.add("--nfsmw_sombras_sin_vegetacion=" + (worldFactor(context, WORLD_SHADOWS) < 1f));
            args.add("--nfsmw_nativo_compuesta_perezosa=" + flag(context, LAZY_COMPOSITE));
            // Off by default: each GPU timestamp makes a phone's GPU wait for the work in flight.
            args.add("--fh2_nativo_medir_gpu=" + flag(context, GPU_MEASUREMENTS));
            // The overlay's GPU % from two timestamps per submission, only where no kernel file has it (Mali on
            // MediaTek with Android 16): the timestamps cost a little GPU time.
            args.add("--fh2_nativo_medir_gpu_uso=" + (flag(context, SHOW_USAGE) && !UsageMonitor.kernelGpuReadable()));
            args.add("--fh2_nativo_cache_vertices=" + flag(context, VERTEX_CACHE));
            args.add("--fh2_nativo_cache_indices=" + flag(context, INDEX_CACHE));
            // FH2's bone and instance pools across frames: no toggle, off where the other two start off.
            args.add("--fh2_nativo_cache_buscas=" + !crossFrameCachesOffByDefault(context));
            args.add("--fh2_nativo_vertices_sin_espera=" + flag(context, VERTEX_NO_WAIT));
            args.add("--fh2_nativo_destinos_exactos=" + flag(context, EXACT_TARGETS));
            args.add("--fh2_nativo_mips_reflejo_juntos=" + flag(context, REFLECTION_MIPS));
            args.add("--fh2_nativo_area_justa=" + flag(context, TIGHT_AREA));
            // Only does something with MSAA: soft but grainy foliage edges instead of the alpha test's hard cut.
            args.add("--fh2_nativo_msaa_a2c=" + flag(context, FOLIAGE_COVERAGE));
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
        // Off by default and marked unstable in the menu. Presenting on its own thread is meant to keep the
        // thread that feeds the GPU recording while the present waits, but measured on 2026-10-26 (S23 and Moto,
        // stopped cars, same scene, on then off) it changed nothing: S23 20.6 vs 21.3 fps with frames over 50 ms
        // 47.9 % vs 35.6 %, Moto 20.6 vs 19.9 fps, and ~20 ms inside the present either way (the FIFO wait of a
        // GPU-bound frame). The Switch measurement (presenter.cpp) also found more frame-time variance with it.
        // Read at start (kRequiresRestart).
        args.add("--present_hilo_propio=" + flag(context, PRESENT_THREAD));
        // The view gives the aspect ratio (16:9 or stretched, GameActivity.layoutSurface) and the presenter
        // fills the whole buffer. The scaling effect (bilinear, CAS sharpening or FSR 1.0) needs
        // REX_HAS_FIDELITYFX_SDK on Android (sdk/src/ui/CMakeLists.txt).
        String scaling = get(context, SCALING);
        // Off: the typed render scale shown as it is, no upscaling filter (nearest sampling: 160x90 is blocky).
        args.add("--present_nearest=" + "off".equals(scaling));
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
        args.addAll(extraArguments(get(context, EXTRA_ARGS)));
        // HDR10 output (vulkan_presenter.cpp, from Pinyon Shift): the game's SDR image with white at the paper white.
        // The display's levels go with it, so the SDK needs no JNI to read them.
        float[] hdr = hdrCapabilities(context);
        if (hdr != null && flag(context, HDR)) {
            args.add("--present_hdr=true");
            args.add("--present_hdr_paper_white_nits=" + hdrPaperWhite(context));
            // Native renderer only: it marks the HUD (nfsmw_nativo_destinos.cpp); the emulated GPU's output stays opaque.
            args.add("--present_hdr_highlights=" + hdrHighlights(context));
            args.add("--present_hdr_display_hdr10=true");
            // The declared peak (HDR_PEAK) and, when it is raised, a frame average in the same proportion.
            float peak = hdrPeakNits(context, hdr[0]);
            args.add("--present_hdr_display_max_nits=" + peak);
            args.add("--present_hdr_display_max_average_nits=" + (hdr[0] > 0f ? hdr[1] * peak / hdr[0] : peak / 2f));
            args.add("--present_hdr_display_min_nits=" + hdr[2]);
            if (hdrRelative(context)) {
                // The white follows the brightness slider and the peak the display's ratio (updated while running).
                args.add("--present_hdr_relative=true");
                args.add("--present_hdr_headroom=" + hdrSdrRatio(context));
            }
        }
        return args;
    }
}
