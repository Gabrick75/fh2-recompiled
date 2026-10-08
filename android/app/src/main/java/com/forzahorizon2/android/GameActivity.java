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

    private final Handler clock = new Handler(Looper.getMainLooper());
    private boolean stretched = true;
    private TextView fpsLabel;

    @Override
    protected void onCreate(Bundle state) {
        File appRoot = new File(getFilesDir(), "fh2");
        gameRoot = new File(Environment.getExternalStorageDirectory(), MainActivity.GAME_FOLDER_NAME).getAbsolutePath();
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
        boolean showFps = GameOptions.flag(this, GameOptions.SHOW_FPS);
        boolean showUsage = GameOptions.flag(this, GameOptions.SHOW_USAGE) ||
                (getIntent() != null && getIntent().getBooleanExtra("fh2.uso", false));
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
            for (File entry : entries) {
                copyTree(entry, new File(target, entry.getName()));
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

    private static void copyTree(File source, File target) throws IOException {
        if (source.isDirectory()) {
            if (!target.mkdirs() && !target.isDirectory()) {
                throw new IOException("mkdir " + target);
            }
            File[] children = source.listFiles();
            if (children != null) {
                for (File child : children) {
                    copyTree(child, new File(target, child.getName()));
                }
            }
        } else {
            java.nio.file.Files.copy(source.toPath(), target.toPath(),
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private static void deleteTree(File file) {
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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Display display = getWindowManager().getDefaultDisplay();
            Display.Mode best = null;
            for (Display.Mode mode : display.getSupportedModes()) {
                if (mode.getPhysicalWidth() != display.getMode().getPhysicalWidth()) {
                    continue;
                }
                // The lowest mode that still shows every frame the game makes.
                if (mode.getRefreshRate() + 0.5f >= fps &&
                        (best == null || mode.getRefreshRate() < best.getRefreshRate())) {
                    best = mode;
                }
            }
            if (best != null) {
                attrs.preferredDisplayModeId = best.getModeId();
            }
        }
        getWindow().setAttributes(attrs);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (inputManager != null) {
            inputManager.registerInputDeviceListener(this, null);
            refreshGamepads();
        }
        updateTiltListener();
        applyStretch(controls.stretch);
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
        UsageMonitor monitor = new UsageMonitor(this);
        Thread reader = new Thread(() -> {
            while (!isFinishing() && !isDestroyed()) {
                String text = monitor.sample();
                clock.post(() -> usageLabel.setText(text));
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
        args.add("--user_data_root=" + userRoot);
        args.add("--cache_root=" + cacheRoot);
        // The launcher's options win over fh2.toml.
        args.addAll(GameOptions.arguments(this));
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
        return args.toArray(new String[0]);
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

    private void refreshGamepads() {
        boolean any = false;
        for (int id : inputManager.getInputDeviceIds()) {
            any |= isGamepad(inputManager.getInputDevice(id));
        }
        if (any) {
            controls.onGamepadActive();
        } else {
            controls.onGamepadGone();
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
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (isGamepad(event.getDevice())) {
            controls.onGamepadActive();
        }
        return super.dispatchGenericMotionEvent(event);
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
        hint.setText("Toque um controle para escolhê-lo · arraste para mover · faça pinça para mudar o tamanho");
        hint.setTextColor(0xCCFFFFFF);
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        hint.setGravity(Gravity.CENTER);
        bar.addView(hint);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.addView(toolButton("Tamanho −", v -> controls.resizeSelected(1f / 1.12f)));
        row.addView(toolButton("Tamanho +", v -> controls.resizeSelected(1.12f)));
        row.addView(toolButton("Mostrar / ocultar", v -> controls.toggleSelectedVisible()));
        row.addView(toolButton("Ajustes", v -> showSettings()));
        row.addView(toolButton("Restaurar", v -> confirmReset()));
        Button done = toolButton("Pronto", v -> controls.setEditing(false));
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
                .setTitle("Restaurar controles")
                .setMessage("Voltar à posição e ao tamanho originais de todos os controles?")
                .setPositiveButton("Restaurar", (d, w) -> controls.resetLayout())
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private void showSettings() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(20), dp(8), dp(20), dp(8));

        panel.addView(toggle("Imagem esticada na tela inteira", controls.stretch, on -> {
            controls.stretch = on;
            applyStretch(on);
        }));
        panel.addView(toggle("Vibrar ao tocar", controls.haptics, on -> controls.haptics = on));
        panel.addView(toggle("Ocultar os controles ao usar um controle físico", controls.hideWithGamepad,
                on -> controls.hideWithGamepad = on));
        panel.addView(toggle("Direção inclinando o celular", controls.tiltSteering, on -> {
            controls.tiltSteering = on;
            updateTiltListener();
        }));
        panel.addView(slider("Sensibilidade da inclinação", 50, 250,
                Math.round(controls.tiltSensitivity * 100), v -> controls.tiltSensitivity = v / 100f));
        panel.addView(slider("Opacidade dos controles", 15, 100,
                Math.round(controls.opacity * 100), v -> {
                    controls.opacity = v / 100f;
                    controls.invalidate();
                }));

        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Ajustes dos controles")
                .setView(panel)
                .setPositiveButton("Fechar", (d, w) -> controls.save())
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
