package com.forzahorizon2.android;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import static com.forzahorizon2.android.AppLanguage.tr;


import org.libsdl.app.SDLActivity;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.List;

public final class GameActivity extends SDLActivity
        implements TouchControlsView.Host, InputManager.InputDeviceListener, SensorEventListener {
    private static final String TAG = "FH2";
    private static final String SETTINGS = "fh2.toml";

    private String gameRoot;
    private String userRoot;
    private String cacheRoot;
    private TouchControlsView controls;
    private LinearLayout toolbar;
    private InputManager inputManager;
    private SensorManager sensorManager;
    private Sensor gravity;

    private static native float nativeFps();
    private static native float nativeFrameMs();
    // The night fill light (0 = off): N of fh2_nativo_luz_noche, set live by the slider in the settings.
    private static native void nativeSetNightLight(float value);
    // One cvar on the running game: "STATE|old value|new value", STATE LIVE, RESTART, TEXT, UNKNOWN or INVALID
    // (android_rendimiento.cpp).
    private static native String nativeSetFlag(String name, String value);
    private static final String NIGHT_LIGHT = "night_light_percent";
    // Relative HDR (android_hdr.cpp): the display's current HDR/SDR ratio, for the presenter's highlights.
    private static native void nativeSetHdrHeadroom(float ratio);

    // Relative HDR's listener of the display's HDR/SDR ratio while the game is in front (null otherwise).
    private Display hdrDisplay;
    private java.util.function.Consumer<Display> hdrRatioListener;

    private final Handler clock = new Handler(Looper.getMainLooper());
    private boolean stretched = true;
    private TextView fpsLabel;
    // When the mouse's pointer capture was last asked for.
    private long lastMouseCapture;
    // A mouse was used since the last one was unplugged.
    private boolean mouseSeen;
    // How often the pointer capture is asked for again, so that it stays on the game's surface.
    private static final long MOUSE_CAPTURE_INTERVAL = 250;

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(AppLanguage.wrap(base));
    }

    @Override
    protected void onCreate(Bundle state) {
        if (state != null) {
            // Configuration changes do not recreate this screen (configChanges in the manifest), so a saved state
            // means the process died (the game closed, or Android reclaimed it) and Android brought the game screen
            // back by itself. The game cannot resume from there, and one that closes while loading would reopen in
            // a loop: go back to the launcher instead.
            Log.w(TAG, "Tela do jogo recriada depois que o processo morreu: volta ao launcher");
            super.onCreate(state);
            finish();
            return;
        }
        File appRoot = new File(getFilesDir(), "fh2");
        // The extracted folder or the disc image itself (GameIso): the SDK mounts either.
        gameRoot = GameIso.gameDataRoot(this).getAbsolutePath();
        userRoot = new File(appRoot, "user").getAbsolutePath();
        cacheRoot = new File(appRoot, "cache").getAbsolutePath();
        new File(userRoot).mkdirs();
        new File(cacheRoot).mkdirs();
        // The native side keeps fh2.toml, the pipeline cache and its logs in the "executable folder", which
        // for app_process would be /system/bin (see GetExecutableFolder in filesystem_posix.cpp).
        try {
            Os.setenv("REX_APP_FOLDER", userRoot, true);
        } catch (ErrnoException e) {
            Log.w(TAG, "REX_APP_FOLDER", e);
        }
        installSettings();
        backUpSaves();
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        super.onCreate(state);
        useWholeScreen();
        hideSystemBars();
        applyNightLight();

        ViewGroup root = findViewById(android.R.id.content);
        controls = new TouchControlsView(this);
        controls.setHost(this);
        root.addView(controls, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.FILL));
        toolbar = buildToolbar();
        toolbar.setVisibility(View.GONE);
        // In the middle of the screen, the one area the default layout leaves free, so no control hides under it.
        FrameLayout.LayoutParams barParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        root.addView(toolbar, barParams);
        stretched = controls.stretch;
        layoutSurface();
        // The bench's fh2.uso shows both lines for that run.
        boolean benchUsage = getIntent() != null && getIntent().getBooleanExtra("fh2.uso", false);
        boolean showFps = GameOptions.flag(this, GameOptions.SHOW_FPS) || benchUsage;
        boolean showUsage = GameOptions.flag(this, GameOptions.SHOW_USAGE) || benchUsage;
        if (showFps || showUsage) {
            LinearLayout overlay = new LinearLayout(this);
            overlay.setOrientation(LinearLayout.VERTICAL);
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START);
            lp.setMargins(dp(12), dp(12), 0, 0);
            root.addView(overlay, lp);
            if (showFps) showFpsCounter(overlay);
            if (showUsage) showUsage(overlay);
        }
        if (!showUsage) {
            startUsageMonitor(null);
        }
        // "Gravar perfil de desempenho (root)": once, in this game, then the option turns itself off.
        if (GameOptions.flag(this, GameOptions.ROOT_PROFILE)) {
            GameOptions.flag(this, GameOptions.ROOT_PROFILE, false);
            RootDiagnostics.profileLater(android.os.Process.myPid(), sessionLogBase(), 60);
        }

        inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        sensorManager = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        if (sensorManager != null) {
            gravity = sensorManager.getDefaultSensor(Sensor.TYPE_GRAVITY);
            if (gravity == null) {
                gravity = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
            }
        }
    }

    /*
     * Safety copy of the player's progress before the game runs: the profile folders (16 hex digits, the XUID, with
     * the saves of every title inside) and achievements, about 8 MB. To Internal storage/jogos/ForzaHorizon2-saves/
     * <date>, keeping the first one and the newest SAVE_BACKUPS. A failure only logs: it never stops the game.
     */
    private static final int SAVE_BACKUPS = 10;

    private void backUpSaves() {
        File[] entries = new File(userRoot).listFiles(f -> f.isDirectory() &&
                (f.getName().matches("[0-9A-Fa-f]{16}") || f.getName().equals("achievements")));
        if (entries == null || entries.length == 0) {
            return;
        }
        File backups = new File(Environment.getExternalStorageDirectory(), MainActivity.GAME_FOLDER_NAME + "-saves");
        String stamp = new java.text.SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", java.util.Locale.ROOT)
                .format(new java.util.Date());
        File target = new File(backups, stamp);
        try {
            SaveTransfer.hideFromGallery(backups);
            for (File entry : entries) {
                SaveTransfer.copyEntry(entry, new File(target, entry.getName()));
            }
            File[] old = backups.listFiles(File::isDirectory);
            if (old != null && old.length > SAVE_BACKUPS + 1) {
                java.util.Arrays.sort(old);  // the names are dates, so they sort by age
                // The first copy is never deleted: it is the progress from before any of the later runs.
                for (int i = 1; i < old.length - SAVE_BACKUPS; i++) {
                    deleteTree(old[i]);
                }
            }
            Log.i(TAG, "Cópia do save em " + target);
        } catch (IOException e) {
            Log.w(TAG, "Não foi possível copiar o save para " + target, e);
        }
    }

    static void copyTree(File source, File target) throws IOException {
        copyTree(source, target, f -> true);
    }

    /** Like copyTree, but skips the files and folders `keep` rejects. */
    static void copyTree(File source, File target, java.io.FileFilter keep) throws IOException {
        if (!keep.accept(source)) return;
        if (source.isDirectory()) {
            if (!target.mkdirs() && !target.isDirectory()) {
                throw new IOException("mkdir " + target);
            }
            File[] children = source.listFiles();
            if (children != null) {
                for (File child : children) {
                    copyTree(child, new File(target, child.getName()), keep);
                }
            }
        } else {
            java.nio.file.Files.copy(source.toPath(), target.toPath(),
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        }
    }

    static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }

    // SDL picks an orientation from the window size when it creates its window; the game is landscape only.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    // The image and the controls use all of the panel, camera cutout included, at its highest refresh rate
    // when the FPS limit asks for more than 60.
    private void useWholeScreen() {
        WindowManager.LayoutParams attrs = getWindow().getAttributes();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            attrs.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        int fps = Integer.parseInt(GameOptions.get(this, GameOptions.FPS));
        if (fps <= 0) {
            fps = 240;  // "Unlimited": the fastest mode the panel has.
        }
        Log.i(TAG, "useWholeScreen: fpsLimit=" + fps);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Display display = getWindowManager().getDefaultDisplay();
            Display.Mode current = display.getMode();
            Log.i(TAG, "useWholeScreen: current display mode=" + current.getPhysicalWidth() + "x" +
                    current.getPhysicalHeight() + " @" + current.getRefreshRate() + " Hz");
            Display.Mode best = null;
            for (Display.Mode mode : display.getSupportedModes()) {
                Log.i(TAG, "useWholeScreen: available mode=" + mode.getPhysicalWidth() + "x" +
                        mode.getPhysicalHeight() + " @" + mode.getRefreshRate() + " Hz id=" + mode.getModeId());
                if (mode.getPhysicalWidth() != current.getPhysicalWidth()) {
                    continue;
                }
                // The lowest mode that still shows every frame the game makes.
                if (mode.getRefreshRate() + 0.5f >= fps &&
                        (best == null || mode.getRefreshRate() < best.getRefreshRate())) {
                    best = mode;
                }
            }
            if (best != null) {
                Log.i(TAG, "useWholeScreen: selected mode=" + best.getPhysicalWidth() + "x" +
                        best.getPhysicalHeight() + " @" + best.getRefreshRate() + " Hz id=" + best.getModeId());
                attrs.preferredDisplayModeId = best.getModeId();
            } else {
                // Fallback: Free (ou um alvo acima do que o painel suporta) - escolhe o modo de refresh mais
                // alto da mesma largura física, para não deixar a tela presa em 60 Hz.
                Display.Mode fastest = null;
                for (Display.Mode mode : display.getSupportedModes()) {
                    if (mode.getPhysicalWidth() != current.getPhysicalWidth()) {
                        continue;
                    }
                    if (fastest == null || mode.getRefreshRate() > fastest.getRefreshRate()) {
                        fastest = mode;
                    }
                }
                if (fastest != null) {
                    Log.w(TAG, "useWholeScreen: NO mode >=" + fps + " Hz at " +
                            current.getPhysicalWidth() + "px width; falling back to fastest mode " +
                            fastest.getPhysicalWidth() + "x" + fastest.getPhysicalHeight() +
                            " @" + fastest.getRefreshRate() + " Hz id=" + fastest.getModeId());
                    attrs.preferredDisplayModeId = fastest.getModeId();
                } else {
                    Log.w(TAG, "useWholeScreen: NO display mode found >=" + fps + " Hz at " +
                            current.getPhysicalWidth() + "px width; display stays at " +
                            current.getRefreshRate() + " Hz");
                }
            }
        }
        getWindow().setAttributes(attrs);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    /*
     * The status and navigation bars hidden, the image and the controls under where they were. SDL hides them with
     * the deprecated setSystemUiVisibility flags, which some phones ignore or undo (POCO F6 with HyperOS: a black
     * status-bar-high band over the top of the game, controls and FPS included, and the gesture pill shown). Android
     * 11+ gets the current API; earlier versions keep SDL's flags. Applied again on every return to focus, after SDL.
     */
    private void hideSystemBars() {
        android.view.Window window = getWindow();
        window.setStatusBarColor(Color.TRANSPARENT);
        window.setNavigationBarColor(Color.TRANSPARENT);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            android.view.WindowInsetsController bars = window.getInsetsController();
            if (bars != null) {
                bars.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                bars.hide(android.view.WindowInsets.Type.statusBars() | android.view.WindowInsets.Type.navigationBars());
            }
            WindowManager.LayoutParams attrs = window.getAttributes();
            if (attrs.layoutInDisplayCutoutMode != WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS) {
                attrs.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
                window.setAttributes(attrs);
            }
        }
        // What the system really did, once it has settled, in the session's usage log.
        window.getDecorView().postDelayed(this::logScreenInsets, 500);
    }

    private String lastScreenInsets;

    // "[tela]" line in <session>_uso.log when it changes: whether the bars are visible, the room they take, the
    // cutout. A report from a phone where the band is still there then says why.
    private void logScreenInsets() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;
        android.view.WindowInsets insets = getWindow().getDecorView().getRootWindowInsets();
        if (insets == null) return;
        android.graphics.Insets bars = insets.getInsets(android.view.WindowInsets.Type.systemBars());
        android.view.DisplayCutout cutout = insets.getDisplayCutout();
        View decor = getWindow().getDecorView();
        String line = "[tela] janela " + decor.getWidth() + "x" + decor.getHeight()
                + ", barra de status " + (insets.isVisible(android.view.WindowInsets.Type.statusBars()) ? "VISÍVEL" : "escondida")
                + ", navegação " + (insets.isVisible(android.view.WindowInsets.Type.navigationBars()) ? "VISÍVEL" : "escondida")
                + ", espaço das barras topo " + bars.top + " baixo " + bars.bottom + " esq " + bars.left + " dir " + bars.right
                + ", recorte " + (cutout == null ? "nenhum" : "topo " + cutout.getSafeInsetTop() + " baixo "
                + cutout.getSafeInsetBottom() + " esq " + cutout.getSafeInsetLeft() + " dir " + cutout.getSafeInsetRight());
        if (line.equals(lastScreenInsets)) return;
        lastScreenInsets = line;
        Log.i(TAG, line);
        try (java.io.FileWriter writer = new java.io.FileWriter(sessionLogBase() + "_uso.log", true)) {
            writer.write(new java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.ROOT).format(new java.util.Date())
                    + "  " + line + "\n");
        } catch (IOException ignored) {
            // Logcat has it.
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        // The game asks for the pointer capture again when the app comes back to the front; the
        // surface has to have it too, so the next mouse event asks for it again.
        lastMouseCapture = 0;
        if (inputManager != null) {
            inputManager.registerInputDeviceListener(this, null);
            refreshGamepads();
        }
        updateTiltListener();
        applyStretch(controls.stretch);
        startRelativeHdr();
    }

    /*
     * Relative HDR (GameOptions.hdrRelative, present_hdr_relative in the presenter). The game's surface asks Android
     * for HDR headroom above the SDR white, and the display's ratio, which follows the brightness slider and the ambient
     * light, reaches the presenter whenever it changes: the highlights go up to the peak the display gives right now.
     */
    @android.annotation.TargetApi(35)
    private void startRelativeHdr() {
        if (Build.VERSION.SDK_INT < 35 || hdrRatioListener != null || !GameOptions.hdrRelative(this)) return;
        if (mSurface != null) mSurface.setDesiredHdrHeadroom(GameOptions.HDR_DESIRED_HEADROOM);
        hdrDisplay = getWindowManager().getDefaultDisplay();
        hdrRatioListener = display -> sendHdrHeadroom(display.getHdrSdrRatio());
        hdrDisplay.registerHdrSdrRatioChangedListener(getMainExecutor(), hdrRatioListener);
        sendHdrHeadroom(hdrDisplay.getHdrSdrRatio());
    }

    @android.annotation.TargetApi(35)
    private void stopRelativeHdr() {
        if (Build.VERSION.SDK_INT < 35 || hdrRatioListener == null) return;
        hdrDisplay.unregisterHdrSdrRatioChangedListener(hdrRatioListener);
        hdrRatioListener = null;
        hdrDisplay = null;
    }

    private static void sendHdrHeadroom(float ratio) {
        try {
            nativeSetHdrHeadroom(ratio);
        } catch (UnsatisfiedLinkError e) {
            // The game's library did not load (SDL shows its own error).
        }
    }

    private void applyStretch(boolean stretch) {
        stretched = stretch;
        if (mLayout != null) {
            mLayout.requestLayout();
            resizeSurface(mLayout.getWidth(), mLayout.getHeight());
        }
    }

    /*
     * Display size and aspect, as NFSMW-Recompiled-Mobile does (GameActivity.ajustarSuperficie):
     *  - the view is the screen rectangle the game takes: the largest 16:9 that fits, centred on black, or the
     *    whole screen when stretched;
     *  - the buffer is the pixels presented: 720x480, 1280x720 or the view's own. Android's compositor scales it to
     *    the view for free, so 720x480 in a 16:9 view is anamorphic, like the Xbox 360's widescreen 480p.
     * The presenter fills the whole buffer (present_letterbox=false, GameOptions.arguments).
     *
     * SDL adds its surface with WRAP_CONTENT, and a SurfaceView with setFixedSize in WRAP_CONTENT measures as its
     * buffer (SurfaceView.onMeasure): the view shrank to a corner. So the view always gets an exact size here.
     */
    private void layoutSurface() {
        if (mSurface == null || mLayout == null) {
            return;
        }
        mLayout.setBackgroundColor(Color.BLACK);
        String resolution = getIntent() != null ? getIntent().getStringExtra("fh2.resolucao") : null;
        int[] buffer = GameOptions.bufferSize(resolution != null ? resolution
                                                                 : GameOptions.get(this, GameOptions.RESOLUTION));
        if (buffer[0] > 0) {
            mSurface.getHolder().setFixedSize(buffer[0], buffer[1]);
        } else {
            mSurface.getHolder().setSizeFromLayout();
        }
        // The real room Android leaves (camera cutout, system bars) is only known at layout, and may change.
        mLayout.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> resizeSurface(r - l, b - t));
    }

    private int lastWidth = -1;
    private int lastHeight = -1;
    private boolean lastStretched;

    private void resizeSurface(int width, int height) {
        if (width <= 0 || height <= 0 || (width == lastWidth && height == lastHeight && stretched == lastStretched)) {
            return;
        }
        lastWidth = width;
        lastHeight = height;
        lastStretched = stretched;
        int w = width;
        int h = height;
        if (!stretched) {
            if (width * 9 > height * 16) {
                w = height * 16 / 9;
            } else {
                h = width * 9 / 16;
            }
        }
        android.widget.RelativeLayout.LayoutParams lp = new android.widget.RelativeLayout.LayoutParams(w, h);
        lp.addRule(android.widget.RelativeLayout.CENTER_IN_PARENT);
        // A layout change made from its own listener only applies on the next pass: post it.
        mLayout.post(() -> mSurface.setLayoutParams(lp));
        Log.i(TAG, "Superfície: vista " + w + "x" + h + (stretched ? " (esticada)" : " (16:9)"));
    }

    private TextView overlayLabel(String text) {
        TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setBackgroundColor(0x99000000);
        label.setTypeface(android.graphics.Typeface.MONOSPACE, android.graphics.Typeface.BOLD);
        label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        label.setPadding(dp(6), dp(3), dp(6), dp(3));
        label.setText(text);
        return label;
    }

    // A line under the FPS with the GPU, CPU and RAM use, every second, read off the UI thread.
    private void showUsage(LinearLayout overlay) {
        TextView usageLabel = overlayLabel("GPU --  CPU --  RAM --");
        overlay.addView(usageLabel, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        startUsageMonitor(usageLabel);
    }

    // GPU, CPU and RAM use: on the overlay every second when it is shown, and in the session's usage log
    // (<session>_uso.log, next to the game log) every 5 seconds in any case, so every play session can be read
    // back afterwards.
    private void startUsageMonitor(TextView usageLabel) {
        UsageMonitor monitor = new UsageMonitor(this);
        String usageLog = sessionLogBase() + "_uso.log";
        Thread reader = new Thread(() -> {
            int tick = 0;
            while (!isFinishing() && !isDestroyed()) {
                String text = monitor.sample();
                monitor.sampleClocks();  // the clocks and their caps go to the log only, not the overlay
                if (usageLabel != null) {
                    clock.post(() -> usageLabel.setText(text));
                }
                if (++tick % 5 == 0) {
                    float fps;
                    try {
                        fps = nativeFps();
                    } catch (UnsatisfiedLinkError e) {
                        fps = -1;
                    }
                    String line = new java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.ROOT)
                            .format(new java.util.Date()) +
                            String.format(java.util.Locale.ROOT, "  %.1f fps  ", fps) + text +
                            monitor.clocksLine() + "\n";
                    try (java.io.FileWriter writer = new java.io.FileWriter(usageLog, true)) {
                        writer.write(line);
                    } catch (IOException ignored) {
                        // No log, the overlay still works.
                    }
                    Log.i("fh2", "[uso] " + line.trim());
                }
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException e) {
                    return;
                }
            }
        }, "fh2 uso");
        reader.setDaemon(true);
        reader.start();
    }

    private String sessionLogBase;

    // Internal storage/jogos/fh2-logs/fh2_<date>_<time>: one game log and one usage log per session, readable with
    // adb (the SDK's own log file is in the app's private folder). Keeps the newest 40 files.
    private String sessionLogBase() {
        if (sessionLogBase == null) {
            File dir = new File(Environment.getExternalStorageDirectory(), "jogos/fh2-logs");
            dir.mkdirs();
            File[] old = dir.listFiles();
            if (old != null && old.length > 40) {
                java.util.Arrays.sort(old, (a, b) -> Long.compare(a.lastModified(), b.lastModified()));
                for (int i = 0; i < old.length - 40; i++) {
                    old[i].delete();
                }
            }
            sessionLogBase = new File(dir, "fh2_" + new java.text.SimpleDateFormat("yyyyMMdd_HHmmss",
                    java.util.Locale.ROOT).format(new java.util.Date())).getPath();
        }
        return sessionLogBase;
    }

    // A corner label with the game's FPS and frame time, every half second (NFSMW-Recompiled-Mobile's counter).
    private void showFpsCounter(LinearLayout overlay) {
        fpsLabel = overlayLabel("-- fps");
        overlay.addView(fpsLabel, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        clock.post(new Runnable() {
            // setText relayouts the label even with the same text, and a steady FPS is the usual case.
            private String last = "";

            @Override
            public void run() {
                float fps;
                float ms;
                try {
                    fps = nativeFps();
                    ms = nativeFrameMs();
                } catch (UnsatisfiedLinkError e) {
                    fps = ms = -1;
                }
                String text = fps < 0 ? "-- fps" : String.format(java.util.Locale.ROOT, "%.0f fps  %.1f ms", fps, ms);
                if (!text.equals(last)) {
                    fpsLabel.setText(text);
                    last = text;
                }
                clock.postDelayed(this, 500);
            }
        });
    }

    @Override
    protected void onPause() {
        stopRelativeHdr();
        if (inputManager != null) {
            inputManager.unregisterInputDeviceListener(this);
        }
        if (sensorManager != null) {
            sensorManager.unregisterListener(this);
        }
        if (controls.isEditing()) {
            controls.setEditing(false);
        }
        super.onPause();
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        args.add("--game_data_root=" + gameRoot);
        // From an ISO, the native renderer also looks for fh2_shaders.nfsp in the game folder.
        args.add("--fh2_nativo_biblioteca_dir=" + MainActivity.gameRoot().getAbsolutePath());
        args.add("--user_data_root=" + userRoot);
        args.add("--cache_root=" + cacheRoot);
        // The launcher's options win over fh2.toml.
        args.addAll(GameOptions.arguments(this));
        args.add("--log_file=" + sessionLogBase() + ".log");
        // Test bench (MainActivity.startBenchIfAsked): each option replaces the one of the same name.
        String[] extra = getIntent() != null ? getIntent().getStringArrayExtra("fh2.args") : null;
        if (extra != null) {
            for (String option : extra) {
                int eq = option.indexOf('=');
                String name = eq > 0 ? option.substring(0, eq + 1) : option;
                args.removeIf(a -> a.equals(option) || (eq > 0 && a.startsWith(name)));
                args.add(option);
            }
        }
        writeSessionHeader(args);
        return args.toArray(new String[0]);
    }

    // The session's settings at the top of its game log (written before the game opens it, which then appends)
    // and of its usage log: device, every launcher and touch-control option the player set, and the arguments
    // the game really got.
    private void writeSessionHeader(List<String> args) {
        StringBuilder header = new StringBuilder();
        header.append("===== partida ").append(new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss",
                java.util.Locale.ROOT).format(new java.util.Date())).append(" =====\n");
        try {
            header.append(Diagnostics.summary(this));
        } catch (RuntimeException e) {
            header.append("aparelho: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append('\n');
        }
        header.append("-- opções do launcher\n");
        for (java.util.Map.Entry<String, ?> option :
                new java.util.TreeMap<>(GameOptions.prefs(this).getAll()).entrySet()) {
            header.append(option.getKey()).append(" = ").append(option.getValue()).append('\n');
        }
        header.append("-- controles de toque\n");
        for (java.util.Map.Entry<String, ?> option : new java.util.TreeMap<>(
                getSharedPreferences("fh2_controls", MODE_PRIVATE).getAll()).entrySet()) {
            String value = String.valueOf(option.getValue());
            // The layout of each control is long and says nothing about performance.
            header.append(option.getKey()).append(" = ")
                    .append(value.length() > 60 ? value.substring(0, 60) + "…" : value).append('\n');
        }
        header.append("-- argumentos do jogo\n");
        for (String argument : args) {
            header.append(argument).append('\n');
        }
        header.append("=====\n");
        for (String path : new String[] {sessionLogBase() + ".log", sessionLogBase() + "_uso.log"}) {
            try (java.io.FileWriter writer = new java.io.FileWriter(path, true)) {
                writer.write(header.toString());
            } catch (IOException ignored) {
                // No log; the game still starts.
            }
        }
        Log.i("fh2", header.toString());
    }

    // ---- Physical gamepads ---------------------------------------------------------------------------------

    private static boolean isGamepad(InputDevice device) {
        if (device == null || device.isVirtual()) {
            return false;
        }
        int sources = device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    /*
     * A keyboard or a mouse: the game always reads them as a controller (the SDK's MnK driver), so they are found
     * the way a gamepad is. Phones list keyboards of their own: the volume and power keys, the fingerprint reader
     * and the virtual keyboard of "adb shell input". None of them types letters, which is what a real keyboard
     * does; the one that does and is not a gamepad is the player's.
     */
    private static boolean isKeyboard(InputDevice device) {
        if (device == null || device.isVirtual() || isGamepad(device)) {
            return false;
        }
        return (device.getSources() & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD &&
                device.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC;
    }

    private static boolean isMouse(InputDevice device) {
        if (device == null || device.isVirtual()) {
            return false;
        }
        return (device.getSources() & InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE;
    }

    /*
     * Connected, and not only used: a keyboard or a mouse built into the device (a tablet's cover keyboard, a
     * laptop's touchpad) is taken when it is used, not because it is there. Android 10 tells them apart; before
     * it, only use counts.
     */
    private static boolean isExternal(InputDevice device) {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && device.isExternal();
    }

    private void refreshGamepads() {
        boolean any = false;
        boolean anyKeyboard = false;
        boolean anyMouse = false;
        for (int id : inputManager.getInputDeviceIds()) {
            InputDevice device = inputManager.getInputDevice(id);
            any |= isGamepad(device);
            anyKeyboard |= (isKeyboard(device) || isMouse(device)) && isExternal(device);
            anyMouse |= isMouse(device);
        }
        if (any) {
            controls.onGamepadActive();
        } else {
            controls.onGamepadGone();
        }
        if (anyKeyboard) {
            controls.onKeyboardActive();
        } else {
            controls.onKeyboardGone();
        }
        if (!anyMouse) {
            mouseSeen = false;
        }
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        refreshGamepads();
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        refreshGamepads();
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        refreshGamepads();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (isGamepad(event.getDevice())) {
            controls.onGamepadActive();
        } else if (isKeyboard(event.getDevice()) && !event.isSystem()) {
            controls.onKeyboardActive();
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (isGamepad(event.getDevice())) {
            controls.onGamepadActive();
        }
        if (TouchControlsView.isMouse(event)) {
            // A mouse: the game reads it, and the touch pad gets out of the way.
            onMouseUsed();
            return deliverMouseToSurface(event);
        }
        return super.dispatchGenericMotionEvent(event);
    }

    // ---- The mouse -----------------------------------------------------------------------------------------

    /*
     * The touch pad is drawn over the game's surface, so a mouse that moves over the screen goes to the pad
     * and never reaches the game. The activity hands the mouse to the surface, which is where SDL waits for
     * it (the surface's own touch and motion listeners).
     */
    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        if (TouchControlsView.isMouse(event)) {
            onMouseUsed();
            if (mSurface != null) {
                requestMouseCapture();
                return mSurface.dispatchTouchEvent(event);
            }
            return false;
        }
        return super.dispatchTouchEvent(event);
    }

    /** The mouse's movement, to the game's surface. */
    private boolean deliverMouseToSurface(MotionEvent event) {
        if (mSurface == null) {
            return false;
        }
        requestMouseCapture();
        return mSurface.dispatchGenericMotionEvent(event);
    }

    /** A mouse moved or clicked: the touch pad gets out of the way, and the mouse is the game's from now on. */
    private void onMouseUsed() {
        mouseSeen = true;
        controls.onKeyboardActive();
    }

    /*
     * The pointer capture (Android 8 and later) is what makes the mouse report movement instead of a
     * position, and so the camera follows it. The game asks for it too, but on the layout above the
     * surface, and the surface is the view that reads the mouse: the capture is asked for here, and asked
     * for again from time to time, so that it is the surface's and not the layout's. Only once a mouse was
     * used: without one the pointer stays as it is.
     */
    private void requestMouseCapture() {
        if (mSurface == null || !mouseSeen) {
            return;
        }
        long now = android.os.SystemClock.uptimeMillis();
        if (now - lastMouseCapture < MOUSE_CAPTURE_INTERVAL) {
            return;
        }
        lastMouseCapture = now;
        try {
            mSurface.requestPointerCapture();
        } catch (RuntimeException e) {
            // Not every device allows it: the mouse then moves the camera by the distance it travels.
            Log.w(TAG, "requestPointerCapture: " + e);
        }
    }

    @Override
    public boolean onCapturedMouse(MotionEvent event) {
        // The pad holds the capture: the game's surface is under it and reads the mouse.
        return mSurface != null && mSurface.onCapturedPointerEvent(event);
    }

    // ---- Tilt steering -------------------------------------------------------------------------------------

    private void updateTiltListener() {
        if (sensorManager == null || gravity == null) {
            return;
        }
        sensorManager.unregisterListener(this);
        if (controls.tiltSteering) {
            sensorManager.registerListener(this, gravity, SensorManager.SENSOR_DELAY_GAME);
        }
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        // The "up" of the world in screen coordinates; turning the phone like a wheel rotates it.
        float gx = event.values[0], gy = event.values[1];
        int rotation = getWindowManager().getDefaultDisplay().getRotation();
        float ux, uy;
        if (rotation == Surface.ROTATION_270) {
            ux = -gx;
            uy = -gy;
        } else {
            ux = gx;
            uy = gy;
        }
        double degrees = Math.toDegrees(Math.atan2(uy, ux));
        controls.setTilt((float) (degrees / 30.0));
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
    }

    // ---- Layout editor -------------------------------------------------------------------------------------

    @Override
    public void onEditModeChanged(boolean editing) {
        toolbar.setVisibility(editing ? View.VISIBLE : View.GONE);
    }

    private LinearLayout buildToolbar() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.VERTICAL);
        bar.setGravity(Gravity.CENTER_HORIZONTAL);
        bar.setPadding(dp(10), dp(6), dp(10), dp(8));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xE6121A22);
        bg.setCornerRadius(dp(14));
        bg.setStroke(dp(1), 0x55FFFFFF);
        bar.setBackground(bg);

        TextView hint = new TextView(this);
        hint.setText(tr(R.string.tap_a_control_to_pick_it));
        hint.setTextColor(0xCCFFFFFF);
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        hint.setGravity(Gravity.CENTER);
        bar.addView(hint);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.addView(toolButton(tr(R.string.size_smaller), v -> controls.resizeSelected(1f / 1.12f)));
        row.addView(toolButton(tr(R.string.size_larger), v -> controls.resizeSelected(1.12f)));
        row.addView(toolButton(tr(R.string.show_hide), v -> controls.toggleSelectedVisible()));
        row.addView(toolButton(tr(R.string.settings), v -> showSettings()));
        row.addView(toolButton(tr(R.string.reset), v -> confirmReset()));
        Button done = toolButton(tr(R.string.done), v -> controls.setEditing(false));
        ((GradientDrawable) done.getBackground()).setColor(0xFFE07B00);
        row.addView(done);
        bar.addView(row);
        return bar;
    }

    private Button toolButton(String label, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        b.setMinHeight(0);
        b.setMinimumHeight(0);
        b.setPadding(dp(12), dp(6), dp(12), dp(6));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xFF2A3440);
        bg.setCornerRadius(dp(10));
        b.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(dp(4), dp(6), dp(4), 0);
        b.setLayoutParams(lp);
        b.setOnClickListener(onClick);
        return b;
    }

    private void confirmReset() {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(tr(R.string.reset_the_controls))
                .setMessage(tr(R.string.put_every_control_back_in_its))
                .setPositiveButton(tr(R.string.reset), (d, w) -> controls.resetLayout())
                .setNegativeButton(tr(R.string.cancel), null)
                .show();
    }

    // The saved night light to the renderer (percent / 100 = N of fh2_nativo_luz_noche). Without a saved value the
    // renderer keeps what the launcher's extra options say.
    private void applyNightLight() {
        SharedPreferences prefs = getSharedPreferences("fh2", MODE_PRIVATE);
        if (prefs.contains(NIGHT_LIGHT)) {
            try {
                nativeSetNightLight(prefs.getInt(NIGHT_LIGHT, 0) / 100f);
            } catch (UnsatisfiedLinkError e) {
                Log.w(TAG, "nativeSetNightLight", e);
            }
        }
    }

    /**
     * The launcher's "Extra game options" from inside the game. "Apply now" saves the text there (so what needs a restart
     * counts at the next start) and tries every --name=value word on the running game: the result says, per word, whether
     * it applied now, is saved for the next start, does not exist or was not accepted.
     */
    private void showFlagsDialog() {
        android.widget.EditText field = new android.widget.EditText(this);
        field.setText(GameOptions.get(this, GameOptions.EXTRA_ARGS));
        field.setHint("--name=value --other=value");
        field.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        field.setTypeface(android.graphics.Typeface.MONOSPACE);
        field.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        TextView hint = new TextView(this);
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        hint.setText(tr(R.string.test_flags_description));
        hint.setPadding(0, 0, 0, dp(8));
        TextView result = new TextView(this);
        result.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        result.setTypeface(android.graphics.Typeface.MONOSPACE);
        result.setPadding(0, dp(10), 0, 0);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(20), dp(8), dp(20), dp(8));
        box.addView(hint);
        box.addView(field);
        box.addView(result);
        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.addView(box);
        AlertDialog dialog = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(tr(R.string.test_flags))
                .setView(scroll)
                .setPositiveButton(tr(R.string.apply_now), null)  // set below: it must not close the dialog
                .setNegativeButton(tr(R.string.close), null)
                .show();
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            String text = field.getText().toString().trim();
            GameOptions.set(this, GameOptions.EXTRA_ARGS, text);
            result.setText(applyFlags(text));
        });
    }

    private String applyFlags(String text) {
        StringBuilder out = new StringBuilder();
        for (String word : text.split("\\s+")) {
            if (word.isEmpty()) {
                continue;
            }
            int equals = word.indexOf('=');
            if (!word.startsWith("--") || equals <= 2 || equals >= word.length() - 1) {
                out.append("? ").append(word).append(": ").append(tr(R.string.flag_form)).append('\n');
                continue;
            }
            String name = word.substring(2, equals);
            String value = word.substring(equals + 1);
            String[] parts;
            try {
                parts = nativeSetFlag(name, value).split("\\|", -1);
            } catch (UnsatisfiedLinkError e) {
                parts = new String[] {"UNKNOWN", "", ""};
            }
            switch (parts[0]) {
                case "LIVE":
                    out.append("OK  ").append(name).append(" = ").append(parts[2]).append(" (").append(parts[1])
                            .append("): ").append(tr(R.string.flag_applied)).append('\n');
                    if ("fh2_nativo_luz_noche".equals(name)) {  // the night light slider shows the same value
                        try {
                            getSharedPreferences("fh2", MODE_PRIVATE).edit().putInt(NIGHT_LIGHT,
                                    Math.round(Float.parseFloat(parts[2]) * 100f)).apply();
                        } catch (NumberFormatException ignored) {
                        }
                    }
                    break;
                case "RESTART":
                case "TEXT":
                    out.append("..  ").append(name).append(" = ").append(value).append(": ")
                            .append(tr(R.string.flag_saved)).append('\n');
                    break;
                case "INVALID":
                    out.append("X   ").append(name).append(" = ").append(value).append(": ")
                            .append(tr(R.string.flag_invalid)).append('\n');
                    break;
                default:
                    out.append("?   ").append(name).append(": ").append(tr(R.string.flag_unknown)).append('\n');
                    break;
            }
        }
        if (out.length() == 0) {
            return tr(R.string.test_flags_note);
        }
        return out.append('\n').append(tr(R.string.test_flags_note)).toString();
    }

    private void showSettings() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(20), dp(8), dp(20), dp(8));

        panel.addView(toggle(tr(R.string.image_stretched_to_the_whole_screen), controls.stretch, on -> {
            controls.stretch = on;
            applyStretch(on);
        }));
        panel.addView(toggle(tr(R.string.vibrate_on_touch), controls.haptics, on -> controls.haptics = on));
        panel.addView(toggle(tr(R.string.hide_the_controls_when_a_gamepad), controls.hideWithGamepad,
                on -> controls.hideWithGamepad = on));
        panel.addView(toggle(tr(R.string.steer_by_tilting_the_phone), controls.tiltSteering, on -> {
            controls.tiltSteering = on;
            updateTiltListener();
        }));
        panel.addView(slider(tr(R.string.tilt_sensitivity), 50, 250,
                Math.round(controls.tiltSensitivity * 100), v -> controls.tiltSensitivity = v / 100f));
        panel.addView(slider(tr(R.string.night_light), 0, 200,
                getSharedPreferences("fh2", MODE_PRIVATE).getInt(NIGHT_LIGHT, 0), v -> {
                    getSharedPreferences("fh2", MODE_PRIVATE).edit().putInt(NIGHT_LIGHT, v).apply();
                    applyNightLight();
                }));
        panel.addView(toolButton(tr(R.string.test_flags), v -> showFlagsDialog()));
        panel.addView(slider(tr(R.string.controls_opacity), 15, 100,
                Math.round(controls.opacity * 100), v -> {
                    controls.opacity = v / 100f;
                    controls.invalidate();
                }));

        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(tr(R.string.controls_settings))
                .setView(panel)
                .setPositiveButton(tr(R.string.close), (d, w) -> controls.save())
                .setOnDismissListener(d -> controls.save())
                .show();
    }

    private interface OnToggle {
        void set(boolean on);
    }

    private interface OnValue {
        void set(int value);
    }

    private View toggle(String label, boolean value, OnToggle onToggle) {
        Switch s = new Switch(this);
        s.setText(label);
        s.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        s.setChecked(value);
        s.setPadding(0, dp(8), 0, dp(8));
        s.setOnCheckedChangeListener((v, on) -> onToggle.set(on));
        return s;
    }

    private View slider(String label, int min, int max, int value, OnValue onValue) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(0, dp(8), 0, dp(4));
        TextView title = new TextView(this);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        box.addView(title);
        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(Math.max(0, Math.min(max - min, value - min)));
        title.setText(label + ": " + value + " %");
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar s, int progress, boolean fromUser) {
                title.setText(label + ": " + (progress + min) + " %");
                onValue.set(progress + min);
            }

            @Override
            public void onStartTrackingTouch(SeekBar s) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar s) {
            }
        });
        box.addView(bar);
        return box;
    }

    @Override
    protected void onDestroy() {
        clock.removeCallbacksAndMessages(null);
        super.onDestroy();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    // ---- Settings file -------------------------------------------------------------------------------------

    // Copies the Android settings from the APK. A copy the player edited is kept: it is only replaced while
    // it is still the one a previous APK installed.
    private void installSettings() {
        File target = new File(userRoot, SETTINGS);
        SharedPreferences prefs = getSharedPreferences("fh2", MODE_PRIVATE);
        try {
            byte[] bundled = readAll(getAssets().open(SETTINGS));
            String bundledHash = sha256(bundled);
            String installedHash = prefs.getString("settings_sha256", "");
            if (target.isFile()) {
                String currentHash = sha256(readAll(new FileInputStream(target)));
                if (currentHash.equals(bundledHash) || !currentHash.equals(installedHash)) {
                    return;
                }
            }
            try (OutputStream out = new FileOutputStream(target)) {
                out.write(bundled);
            }
            prefs.edit().putString("settings_sha256", bundledHash).apply();
        } catch (IOException | NoSuchAlgorithmException e) {
            Log.w(TAG, "Não foi possível instalar " + SETTINGS, e);
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        try (InputStream stream = in; ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[16384];
            for (int n; (n = stream.read(buffer)) > 0; ) {
                out.write(buffer, 0, n);
            }
            return out.toByteArray();
        }
    }

    private static String sha256(byte[] data) throws NoSuchAlgorithmException {
        StringBuilder hex = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(data)) {
            hex.append(String.format("%02x", b));
        }
        return hex.toString();
    }
}
