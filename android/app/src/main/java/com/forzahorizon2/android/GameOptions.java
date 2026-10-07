package com.forzahorizon2.android;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.List;

/**
 * The launcher's options. Each one is a game cvar that GameActivity passes on the command line, which wins over
 * fh2.toml. Values must be ones the cvar allows, or the game ignores them.
 *
 * Forza Horizon 2 runs on the emulated Xenos GPU (there is no native renderer yet), so the fixed arguments are the
 * ones nfsmw-android found the emulation needs on Android drivers.
 */
final class GameOptions {
    static final class Option {
        final String key;
        final String title;
        final String cvar;
        final String[] values;
        final String[] labels;
        final String defaultValue;

        Option(String key, String title, String cvar, String defaultValue, String[] values, String[] labels) {
            this.key = key;
            this.title = title;
            this.cvar = cvar;
            this.defaultValue = defaultValue;
            this.values = values;
            this.labels = labels;
        }

        String label(String value) {
            for (int i = 0; i < values.length; i++) {
                if (values[i].equals(value)) {
                    return labels[i];
                }
            }
            return value;
        }
    }

    // GameActivity picks the lowest display refresh rate that shows every frame. The emulated GPU does not reach
    // 60 FPS on a phone yet, so this is fixed until the game has a frame limiter.
    static final String FPS = "fps";
    private static final String FPS_VALUE = "60";

    static final Option[] ALL = {
            new Option("volume", "Volume do jogo", "audio_ganancia_pct", "100",
                    new String[] {"100", "125", "150", "200"},
                    new String[] {"Normal", "Alto", "Muito alto", "Máximo · pode distorcer"}),
    };

    private static final String PREFS = "fh2_game";

    private GameOptions() {
    }

    static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static Option find(String key) {
        for (Option o : ALL) {
            if (o.key.equals(key)) {
                return o;
            }
        }
        throw new IllegalArgumentException(key);
    }

    static String get(Context context, String key) {
        if (FPS.equals(key)) {
            return FPS_VALUE;
        }
        Option o = find(key);
        String value = prefs(context).getString(key, o.defaultValue);
        for (String allowed : o.values) {
            if (allowed.equals(value)) {
                return value;
            }
        }
        return o.defaultValue;
    }

    static void set(Context context, String key, String value) {
        prefs(context).edit().putString(key, value).apply();
    }

    static List<String> arguments(Context context) {
        List<String> args = new ArrayList<>();
        for (Option o : ALL) {
            args.add("--" + o.cvar + "=" + get(context, o.key));
        }
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
        return args;
    }
}
