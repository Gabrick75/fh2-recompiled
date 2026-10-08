package com.forzahorizon2.android;

import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.InputType;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.documentfile.provider.DocumentFile;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.card.MaterialCardView;
import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.radiobutton.MaterialRadioButton;
import com.google.android.material.textfield.MaterialAutoCompleteTextView;
import com.google.android.material.textfield.TextInputLayout;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Launcher: finds the game files in Internal storage/jogos/ForzaHorizon2 (or copies them there from a folder the player
 * picks), checks the edition of default.xex, and starts GameActivity. Adapted from nfsmw-android's launcher.
 *
 * The game folder is the extracted disc (tools/fase1_extraer.py on the PC): default.xex, the two DLLs and media/.
 * It can also be copied straight with adb: adb push assets/game_root /sdcard/jogos/ForzaHorizon2
 */
public final class MainActivity extends AppCompatActivity {
    private static final int REQUEST_GAME_FOLDER = 41;
    private static final int REQUEST_STORAGE_ACCESS = 42;
    private static final int REQUEST_LEGACY_STORAGE = 43;
    private static final int REQUEST_SAVE_REPORT = 44;
    private static final int REQUEST_DRIVER_ZIP = 45;
    private static final String TREE_URI = "tree_uri";
    // Relative to the internal storage root (the "jogos" folder holds the user's games).
    static final String GAME_FOLDER_NAME = "jogos/ForzaHorizon2";

    private static final int ACCENT = 0xFFE6007E;
    private static final int BACKGROUND = 0xFF121212;
    private static final int CARD = 0xFF1E1E1E;
    private static final int GREY = 0xFFAAAAAA;
    private static final int NOT_YET = 0xFFE0B04A;

    private TextView importStatus;
    private LinearLayout checksList;
    private LinearLayout settingsColumn;
    private ScrollView settingsScroll;
    private MaterialButton selectFolder;
    private MaterialButton launchGame;
    private MaterialButton saveReport;
    private File pendingReport;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        setContentView(buildScreen());
        prepareGameFolder();
        startBenchIfAsked(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        startBenchIfAsked(intent);
    }

    // Test bench from adb, without touching the screen (adapted from NFSMW-Recompiled-Mobile):
    //   adb shell am start -n com.forzahorizon2.android/.MainActivity --ez fh2.banco true \
    //       --esa fh2.args "--fh2_teste_botoes=8:start,10:a"
    // starts the game at once with the saved options; each fh2.args option replaces the one of the same
    // name (the cvar parser rejects an option given twice).
    private void startBenchIfAsked(Intent intent) {
        if (intent == null || !intent.getBooleanExtra("fh2.banco", false)) return;
        Intent game = new Intent(this, GameActivity.class);
        String[] extra = intent.getStringArrayExtra("fh2.args");
        if (extra != null) game.putExtra("fh2.args", extra);
        String resolution = intent.getStringExtra("fh2.resolucao");  // 480p, 720p or native, for this run only
        if (resolution != null) game.putExtra("fh2.resolucao", resolution);
        startActivity(game);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (hasStorageAccess()) {
            showGameFolder(null);
        }
    }

    // ---- Screen --------------------------------------------------------------------------------------------
    // The menu follows NFSMW-Recompiled-Mobile's (SetupActivity): cards with a title, and each setting with a line
    // below saying what it does. Landscape: the game on the left, the settings on the right.

    private View buildScreen() {
        LinearLayout columns = new LinearLayout(this);
        columns.setOrientation(LinearLayout.HORIZONTAL);
        columns.setBackgroundColor(BACKGROUND);
        columns.setPadding(dp(12), dp(8), dp(12), 0);

        LinearLayout left = column();
        ScrollView leftScroll = new ScrollView(this);
        leftScroll.addView(left);
        columns.addView(leftScroll, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 0.9f));

        LinearLayout right = column();
        settingsScroll = new ScrollView(this);
        settingsScroll.addView(right);
        LinearLayout.LayoutParams rightParams =
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1.1f);
        rightParams.leftMargin = dp(8);
        columns.addView(settingsScroll, rightParams);

        TextView title = new TextView(this);
        title.setText("Forza Horizon 2");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 28);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(Color.WHITE);
        title.setPadding(dp(8), dp(12), dp(8), dp(2));
        left.addView(title);
        TextView subtitle = summary("Xbox 360 · recompilado para Android. Este APK contém o jogo: é só para os seus aparelhos.");
        subtitle.setPadding(dp(8), 0, dp(8), dp(8));
        left.addView(subtitle);

        // --- Game
        importStatus = new TextView(this);
        importStatus.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        importStatus.setTextColor(Color.WHITE);
        checksList = new LinearLayout(this);
        checksList.setOrientation(LinearLayout.VERTICAL);
        checksList.setPadding(0, dp(6), 0, 0);
        launchGame = new MaterialButton(this);
        launchGame.setText("Jogar");
        launchGame.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        launchGame.setTypeface(Typeface.DEFAULT_BOLD);
        launchGame.setMinHeight(dp(64));
        primaryButton(launchGame);
        launchGame.setVisibility(View.GONE);
        launchGame.setOnClickListener(view -> play());
        selectFolder = new MaterialButton(this);
        selectFolder.setText("Escolher pasta do jogo");
        secondaryButton(selectFolder);
        selectFolder.setOnClickListener(view -> selectGameFolder());
        LinearLayout.LayoutParams playParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        playParams.topMargin = dp(12);
        launchGame.setLayoutParams(playParams);
        left.addView(card(section("Jogo"), importStatus, checksList, launchGame, space(4), selectFolder));

        // --- Save (export / import come later: the buttons only show where they will be)
        MaterialButton exportSave = new MaterialButton(this);
        exportSave.setText("Exportar save");
        secondaryButton(exportSave);
        exportSave.setOnClickListener(v -> toast("Exportar o save ainda não foi implementado."));
        MaterialButton importSave = new MaterialButton(this);
        importSave.setText("Importar save");
        secondaryButton(importSave);
        importSave.setOnClickListener(v -> toast("Importar um save ainda não foi implementado."));
        left.addView(card(section("Save do jogo"),
                summary("Copie o seu progresso (perfil e saves) para um arquivo .zip, ou traga de volta um que você exportou antes."),
                space(8), buttonRow(exportSave, importSave)));

        // --- Diagnostics
        saveReport = new MaterialButton(this);
        saveReport.setText("Salvar diagnóstico (ZIP)");
        secondaryButton(saveReport);
        saveReport.setOnClickListener(view -> prepareReport());
        left.addView(card(section("Diagnóstico"),
                summary("Logs e informações do aparelho num .zip, para investigar um problema. Nada é enviado."),
                space(8), saveReport));

        settingsColumn = right;
        buildSettings();
        return columns;
    }

    private void buildSettings() {
        LinearLayout right = settingsColumn;
        right.removeAllViews();

        // --- Display
        right.addView(card(section("Tela"),
                resolutionSelector(),
                toggle("Esticar para a tela inteira",
                        "Desligado: a imagem 16:9 do Xbox 360, com barras pretas. Ligado: preenche a tela, mas distorce a imagem.",
                        GameOptions.stretch(this), v -> GameOptions.stretch(this, v)),
                toggle("Mostrar FPS", "Contador no canto: quadros por segundo e milissegundos por quadro.",
                        GameOptions.flag(this, GameOptions.SHOW_FPS), v -> GameOptions.flag(this, GameOptions.SHOW_FPS, v)),
                selector(GameOptions.PRESENT_MODE, "Modo de apresentação",
                        "FIFO espera o vsync da tela: é o mais rápido (no S23 Ultra, 11 fps contra 7 no menu) e esquenta " +
                                "menos. MAILBOX repinta sem parar: um pouco menos de atraso, mas tira CPU e GPU do jogo.",
                        new String[] {"FIFO (vsync) · mais rápido", "MAILBOX"}, new String[] {"fifo", "mailbox"}, false)));

        // --- Graphics
        right.addView(card(section("Gráficos"),
                notYet(selector(GameOptions.RENDER_RESOLUTION, "Resolução de renderização",
                        "A que o jogo desenha antes de ir para a tela. 1280×720 é a do Xbox 360.",
                        new String[] {"1024×576 · mais rápida", "1280×720 · Xbox 360", "1920×1080 · mais nítida"},
                        new String[] {"1024x576", "1280x720", "1920x1080"}, false)),
                notYet(dropdown(GameOptions.FPS, "Limite de FPS",
                        "O teto do jogo. 90, 120 e Sem limite são experimentais e pedem uma tela com essa taxa. Por enquanto só escolhe a taxa de atualização da tela.",
                        new String[] {"30 fps", "60 fps", "90 fps (experimental)", "120 fps (experimental)", "Sem limite (experimental)"},
                        new String[] {"30", "60", "90", "120", "0"})),
                antiAliasing(),
                notYet(selector(GameOptions.SCALING, "Escala para a tela",
                        "Como a imagem do jogo é levada ao tamanho da tela. O FSR rende mais quando o jogo desenha abaixo da resolução da tela.",
                        new String[] {"Bilinear (o de sempre)", "AMD FSR 1.0 · escala mais nítida", "AMD CAS · só nitidez"},
                        new String[] {"bilinear", "fsr", "cas"}, false)),
                selector(GameOptions.ANISOTROPIC, "Filtro anisotrópico",
                        "Texturas mais nítidas vistas em ângulo, como a estrada ao longe.",
                        new String[] {"Não", "2x", "4x", "16x"}, new String[] {"0", "2", "3", "5"}, true),
                notYet(selector(GameOptions.SHADOWS, "Sombras",
                        "A cada quanto os mapas de sombras são redesenhados. A cada 2 quadros custa menos e quase não se nota.",
                        new String[] {"Todo quadro", "A cada 2 quadros"}, new String[] {"1", "2"}, true)),
                notYet(selector(GameOptions.SHADOW_DISTANCE, "Distância das sombras",
                        "Até onde os objetos projetam sombra. É aí que está quase todo o custo das sombras.",
                        new String[] {"Longe (como o jogo)", "Média", "Perto · mais rápido"},
                        new String[] {"far", "medium", "near"}, false)),
                notYet(selector(GameOptions.CAR_REFLECTIONS, "Reflexos do carro",
                        "Faces do mapa de reflexos do carro redesenhadas a cada quadro.",
                        new String[] {"Altos", "Médios", "Baixos · mais rápido"},
                        new String[] {"high", "medium", "low"}, false))));

        // --- Performance
        right.addView(card(section("Desempenho"),
                toggle("Fixar threads nos núcleos",
                        "Deixa o jogo só nos núcleos rápidos do processador, longe dos núcleos econômicos.",
                        GameOptions.flag(this, GameOptions.PIN_THREADS), v -> GameOptions.flag(this, GameOptions.PIN_THREADS, v)),
                driverSection()));

        // --- Sound
        right.addView(card(section("Som"),
                selector(GameOptions.VOLUME, "Volume do jogo", "Acima de Normal o som pode distorcer nos picos.",
                        new String[] {"Normal", "Alto", "Muito alto", "Máximo"},
                        new String[] {"100", "125", "150", "200"}, false)));
        right.addView(space(12));
    }

    // Display resolution: 480p and 720p only when the screen is bigger than them, plus the native one.
    private View resolutionSelector() {
        DisplayMetrics metrics = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
        int longSide = Math.max(metrics.widthPixels, metrics.heightPixels);
        int shortSide = Math.min(metrics.widthPixels, metrics.heightPixels);
        List<String> labels = new ArrayList<>();
        List<String> values = new ArrayList<>();
        if (shortSide > 480) {
            labels.add("480p · 720×480");
            values.add(GameOptions.RES_480P);
        }
        if (shortSide > 720) {
            labels.add("720p · 1280×720");
            values.add(GameOptions.RES_720P);
        }
        labels.add("Nativa · " + longSide + "×" + shortSide);
        values.add(GameOptions.RES_NATIVE);
        return selector(GameOptions.RESOLUTION, "Resolução",
                "Tamanho da imagem final na tela. O jogo é renderizado do mesmo jeito.",
                labels.toArray(new String[0]), values.toArray(new String[0]), false);
    }

    // Off / MSAA / FXAA, and below only the settings of the chosen one.
    private View antiAliasing() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        View msaa = selector(GameOptions.MSAA_SAMPLES, "Amostras do MSAA",
                "4x é o que o jogo usa. 2x suaviza menos e custa menos.",
                new String[] {"2x", "4x"}, new String[] {"2", "4"}, true);
        View fxaa = selector(GameOptions.FXAA_QUALITY, "Qualidade do FXAA",
                "Alta suaviza mais bordas, inclusive as fracas, e deixa a imagem um pouco mais suave.",
                new String[] {"Normal", "Alta"}, new String[] {"normal", "high"}, true);
        Runnable show = () -> {
            String mode = GameOptions.get(this, GameOptions.ANTI_ALIASING);
            msaa.setVisibility("msaa".equals(mode) ? View.VISIBLE : View.GONE);
            fxaa.setVisibility("fxaa".equals(mode) ? View.VISIBLE : View.GONE);
        };
        box.addView(selector(GameOptions.ANTI_ALIASING, "Suavização de bordas",
                "Tira o serrilhado. O MSAA 4x é o do jogo e o mais caro para a GPU. No S23 Ultra, menu do evento: " +
                        "MSAA 4x 6,8 fps, 2x 9,3, desligado 13,0. O FXAA é um filtro sobre a imagem final, barato.",
                new String[] {"Desligado", "MSAA", "FXAA"}, new String[] {"off", "msaa", "fxaa"}, true, v -> show.run()));
        box.addView(msaa);
        box.addView(fxaa);
        show.run();
        return box;
    }

    // System driver or an imported one (Turnip), import / delete, and the turbo.
    private View driverSection() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.addView(heading("Driver da GPU"));
        String gpu = GpuDrivers.gpuName(this);
        box.addView(summary(GpuDrivers.isAdreno(this)
                ? "Turnip é o driver livre do Mesa para Adreno. Importe o .zip de um driver Turnip (com meta.json) e escolha-o aqui."
                : "Turnip, o driver livre do Mesa, é só para GPUs Adreno: com esta GPU (" +
                  (gpu.isEmpty() ? "desconhecida" : gpu) + ") o jogo usa o driver do sistema."));
        box.addView(notYetNote("Por enquanto o jogo sempre usa o driver do sistema."));

        List<GpuDrivers.Driver> drivers = GpuDrivers.list(this);
        String chosen = GameOptions.get(this, GameOptions.DRIVER);
        if (GpuDrivers.find(this, chosen) == null) chosen = "";
        RadioGroup group = new RadioGroup(this);
        group.setPadding(0, dp(8), 0, 0);
        addRadio(group, gpu.isEmpty() ? "Driver do sistema" : "Driver do sistema (" + gpu + ")", "", chosen);
        for (GpuDrivers.Driver driver : drivers) addRadio(group, driver.name, driver.name, chosen);
        group.setOnCheckedChangeListener((g, id) -> {
            View button = g.findViewById(id);
            if (button != null && ((MaterialRadioButton) button).isChecked()) {
                GameOptions.set(this, GameOptions.DRIVER, (String) button.getTag());
                buildSettingsKeepingScroll();
            }
        });
        box.addView(group);

        MaterialButton importDriver = new MaterialButton(this);
        importDriver.setText("Importar driver (.zip)");
        secondaryButton(importDriver);
        importDriver.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("application/zip");
            startActivityForResult(pick, REQUEST_DRIVER_ZIP);
        });
        MaterialButton deleteDriver = new MaterialButton(this);
        deleteDriver.setText("Apagar o selecionado");
        dangerButton(deleteDriver);
        GpuDrivers.Driver selected = GpuDrivers.find(this, chosen);
        deleteDriver.setEnabled(selected != null);
        deleteDriver.setOnClickListener(v -> {
            GpuDrivers.Driver driver = GpuDrivers.find(this, GameOptions.get(this, GameOptions.DRIVER));
            if (driver != null) {
                GpuDrivers.remove(driver);
                GameOptions.set(this, GameOptions.DRIVER, "");
                buildSettingsKeepingScroll();
            }
        });
        box.addView(buttonRow(importDriver, deleteDriver));

        // The turbo locks the GPU clock for the whole phone: only with a Turnip chosen, as in the fork.
        boolean turnip = selected != null && selected.isTurnip();
        View turbo = notYet(toggle("Turbo da GPU",
                "Fixa o clock da GPU em vez de deixar o sistema ajustá-lo. Só com Turnip em uma GPU Adreno. " +
                        "Nem sempre ajuda: confira com o contador de FPS.",
                turnip && GameOptions.flag(this, GameOptions.TURBO), v -> GameOptions.flag(this, GameOptions.TURBO, v)));
        setEnabledDeep(turbo, turnip);
        box.addView(turbo);
        return box;
    }

    private void addRadio(RadioGroup group, String text, String value, String chosen) {
        MaterialRadioButton button = radio(text);
        button.setTag(value);
        group.addView(button);
        if (value.equals(chosen)) button.setChecked(true);
    }

    private void buildSettingsKeepingScroll() {
        int y = settingsScroll.getScrollY();
        buildSettings();
        settingsScroll.post(() -> settingsScroll.scrollTo(0, y));
    }

    private void importDriver(Uri zip) {
        worker.execute(() -> {
            try {
                GpuDrivers.Driver driver = GpuDrivers.importZip(this, zip);
                mainHandler.post(() -> {
                    GameOptions.set(this, GameOptions.DRIVER, driver.name);
                    buildSettingsKeepingScroll();
                    toast("Driver importado: " + driver.name);
                });
            } catch (IOException error) {
                mainHandler.post(() -> toast("Não foi possível importar o driver: " + error.getMessage()));
            }
        });
    }

    // ---- Play ----------------------------------------------------------------------------------------------

    private void play() {
        launchGame.setEnabled(false);
        Diagnostics.recordLaunch(this);
        worker.execute(() -> {
            GameEdition edition = GameEdition.of(this, gameRoot());
            mainHandler.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                launchGame.setEnabled(true);
                if (edition != null && !edition.supported()) {
                    new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle("Edição do jogo incompatível")
                            .setMessage("Este app foi recompilado a partir da edição " + GameEdition.SUPPORTED_NAME
                                    + " e o seu default.xex é de outra (SHA-256 " + edition.sha256.substring(0, 12)
                                    + "…). Com outra edição o jogo não roda.\n\nUse uma cópia da edição "
                                    + GameEdition.SUPPORTED_NAME + " (o SHA-256 do default.xex começa com "
                                    + GameEdition.SUPPORTED_SHA256.substring(0, 12) + ").")
                            .setPositiveButton("Voltar", null)
                            .setNeutralButton("Jogar mesmo assim",
                                    (dialog, which) -> startActivity(new Intent(this, GameActivity.class)))
                            .show();
                    return;
                }
                startActivity(new Intent(this, GameActivity.class));
            });
        });
    }

    // ---- Game folder ---------------------------------------------------------------------------------------

    static File gameRoot() {
        return new File(Environment.getExternalStorageDirectory(), GAME_FOLDER_NAME);
    }

    private static boolean isValidGameFolder(File folder) {
        return new File(folder, "default.xex").isFile() && new File(folder, "media").isDirectory() &&
                new File(folder, "XMediaFacade_default.xex").isFile() &&
                new File(folder, "SpeechFacade_default.xex").isFile();
    }

    private void showChecks(File folder) {
        checksList.removeAllViews();
        addCheck("default.xex", new File(folder, "default.xex").isFile());
        addCheck("XMediaFacade_default.xex", new File(folder, "XMediaFacade_default.xex").isFile());
        addCheck("SpeechFacade_default.xex", new File(folder, "SpeechFacade_default.xex").isFile());
        addCheck("Pasta media", new File(folder, "media").isDirectory());
    }

    private void addCheck(String name, boolean ok) {
        TextView row = summary((ok ? "✓  " : "✗  ") + name);
        row.setTextColor(ok ? 0xFF7CD992 : 0xFFFF6B6B);
        row.setPadding(0, dp(2), 0, dp(2));
        checksList.addView(row);
    }

    private boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) return Environment.isExternalStorageManager();
        return checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
                PackageManager.PERMISSION_GRANTED;
    }

    private void prepareGameFolder() {
        if (hasStorageAccess()) {
            File root = gameRoot();
            if (!root.isDirectory()) root.mkdirs();
            showGameFolder(null);
            return;
        }
        selectFolder.setEnabled(false);
        launchGame.setVisibility(View.GONE);
        setImportStatus("Para usar Armazenamento interno/" + GAME_FOLDER_NAME +
                ", ative o acesso a arquivos que o Android pede.");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            startActivityForResult(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())), REQUEST_STORAGE_ACCESS);
        } else {
            requestPermissions(new String[] {android.Manifest.permission.WRITE_EXTERNAL_STORAGE},
                    REQUEST_LEGACY_STORAGE);
        }
    }

    private void showGameFolder(String notice) {
        File root = gameRoot();
        selectFolder.setEnabled(true);
        showChecks(root);
        if (isValidGameFolder(root)) {
            setImportStatus((notice == null ? "" : notice + "\n") + "Armazenamento interno/" + GAME_FOLDER_NAME);
            launchGame.setVisibility(View.VISIBLE);
            selectFolder.setText("Trocar pasta do jogo");
        } else {
            setImportStatus("Escolha a pasta extraída do jogo (com default.xex, as duas DLLs .xex e media). " +
                    "Ela será copiada para Armazenamento interno/" + GAME_FOLDER_NAME + " (7,3 GB).");
            launchGame.setVisibility(View.GONE);
            selectFolder.setText("Escolher pasta do jogo");
        }
    }

    private void selectGameFolder() {
        if (!hasStorageAccess()) {
            prepareGameFolder();
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        String saved = getPreferences(MODE_PRIVATE).getString(TREE_URI, null);
        if (saved != null) {
            intent.putExtra("android.provider.extra.INITIAL_URI", Uri.parse(saved));
        }
        startActivityForResult(intent, REQUEST_GAME_FOLDER);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_DRIVER_ZIP) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) importDriver(data.getData());
            return;
        }
        if (requestCode == REQUEST_SAVE_REPORT) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) finishReportSave(data.getData());
            else pendingReport = null;
            return;
        }
        if (requestCode == REQUEST_STORAGE_ACCESS || requestCode == REQUEST_LEGACY_STORAGE) {
            if (hasStorageAccess()) prepareGameFolder();
            else setImportStatus("Ative a permissão de armazenamento para usar Armazenamento interno/" +
                    GAME_FOLDER_NAME + ".");
            return;
        }
        if (requestCode != REQUEST_GAME_FOLDER || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri tree = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException error) {
            setImportStatus("Não foi possível guardar a permissão da pasta: " + error.getMessage());
            return;
        }
        getPreferences(MODE_PRIVATE).edit().putString(TREE_URI, tree.toString()).apply();
        importGameFolder(tree);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_LEGACY_STORAGE && hasStorageAccess()) prepareGameFolder();
    }

    // Copies the picked folder into a staging folder and only swaps it in once it is complete and valid, so an
    // interrupted import never leaves a half game in place.
    private void importGameFolder(Uri tree) {
        selectFolder.setEnabled(false);
        launchGame.setVisibility(View.GONE);
        setImportStatus("Copiando o jogo para Armazenamento interno/" + GAME_FOLDER_NAME + "...");
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        worker.execute(() -> {
            File root = gameRoot();
            File parent = root.getParentFile();
            File staging = new File(parent, "." + root.getName() + ".importing");
            File previous = new File(parent, "." + root.getName() + ".previous");
            try {
                deleteRecursively(staging);
                deleteRecursively(previous);
                DocumentFile source = DocumentFile.fromTreeUri(this, tree);
                if (source == null || !source.isDirectory()) throw new IOException("Não foi possível abrir a pasta.");
                copyDirectory(source, staging, staging.getCanonicalFile(), new long[] {0});
                if (!isValidGameFolder(staging)) {
                    throw new IOException("A pasta deve conter default.xex, XMediaFacade_default.xex, " +
                            "SpeechFacade_default.xex e media.");
                }
                if (root.exists()) Files.move(root.toPath(), previous.toPath(), StandardCopyOption.REPLACE_EXISTING);
                Files.move(staging.toPath(), root.toPath(), StandardCopyOption.REPLACE_EXISTING);
                deleteRecursively(previous);
                mainHandler.post(() -> showGameFolder("Jogo copiado."));
            } catch (Exception error) {
                try { deleteRecursively(staging); } catch (IOException ignored) { }
                mainHandler.post(() -> {
                    setImportStatus("Erro ao importar: " + error.getMessage());
                    selectFolder.setEnabled(true);
                });
            } finally {
                mainHandler.post(() -> getWindow().clearFlags(
                        android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
            }
        });
    }

    private void copyDirectory(DocumentFile source, File destination, File safeRoot, long[] copiedBytes)
            throws IOException {
        if (!destination.getCanonicalFile().toPath().startsWith(safeRoot.toPath())) {
            throw new IOException("A pasta do jogo contém um caminho inválido.");
        }
        if (!destination.mkdirs() && !destination.isDirectory()) throw new IOException("Não foi possível criar " + destination);
        for (DocumentFile child : source.listFiles()) {
            String name = child.getName();
            if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") ||
                    name.contains("/") || name.contains("\\")) {
                throw new IOException("A pasta do jogo contém um nome inválido.");
            }
            File target = new File(destination, name);
            if (child.isDirectory()) {
                copyDirectory(child, target, safeRoot, copiedBytes);
            } else if (child.isFile()) {
                try (InputStream input = getContentResolver().openInputStream(child.getUri());
                     FileOutputStream output = new FileOutputStream(target)) {
                    if (input == null) throw new IOException("Não foi possível ler " + name);
                    byte[] buffer = new byte[1024 * 1024];
                    for (int count; (count = input.read(buffer)) != -1; ) {
                        output.write(buffer, 0, count);
                        copiedBytes[0] += count;
                        if ((copiedBytes[0] & ((64L * 1024 * 1024) - 1)) < count) {
                            long total = copiedBytes[0];
                            mainHandler.post(() -> setImportStatus("Copiados " + (total / (1024 * 1024)) +
                                    " MiB de ~7500..."));
                        }
                    }
                    output.getFD().sync();
                }
            }
        }
    }

    private static void deleteRecursively(File file) throws IOException {
        if (!file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteRecursively(child);
        if (!file.delete()) throw new IOException("Não foi possível apagar " + file.getName());
    }

    // ---- Diagnostic report (saved by the player; never sent anywhere) --------------------------------------

    private void prepareReport() {
        saveReport.setEnabled(false);
        saveReport.setText("Preparando diagnóstico…");
        worker.execute(() -> {
            try {
                File report = Diagnostics.createReport(this);
                mainHandler.post(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    saveReport.setEnabled(true);
                    saveReport.setText("Salvar diagnóstico (ZIP)");
                    pendingReport = report;
                    Intent save = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                    save.addCategory(Intent.CATEGORY_OPENABLE);
                    save.setType("application/zip");
                    save.putExtra(Intent.EXTRA_TITLE, report.getName());
                    try {
                        startActivityForResult(save, REQUEST_SAVE_REPORT);
                    } catch (ActivityNotFoundException error) {
                        pendingReport = null;
                        message("Não há um app para salvar arquivos.");
                    }
                });
            } catch (Exception error) {
                mainHandler.post(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    saveReport.setEnabled(true);
                    saveReport.setText("Salvar diagnóstico (ZIP)");
                    message("Não foi possível preparar o relatório: " + error.getMessage());
                });
            }
        });
    }

    private void finishReportSave(Uri destination) {
        File report = pendingReport;
        pendingReport = null;
        if (report == null || !report.isFile()) {
            message("O relatório não está mais disponível. Gere outro.");
            return;
        }
        worker.execute(() -> {
            try (InputStream input = new FileInputStream(report);
                 OutputStream output = getContentResolver().openOutputStream(destination, "w")) {
                if (output == null) throw new IOException("Não foi possível abrir o arquivo de destino.");
                byte[] buffer = new byte[16384];
                for (int count; (count = input.read(buffer)) != -1; ) output.write(buffer, 0, count);
                mainHandler.post(() -> message("Diagnóstico salvo."));
            } catch (Exception error) {
                mainHandler.post(() -> message("Não foi possível salvar o ZIP: " + error.getMessage()));
            }
        });
    }

    private void message(String text) {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Diagnóstico").setMessage(text).setPositiveButton("OK", null).show();
    }

    // ---- Widgets (as in NFSMW-Recompiled-Mobile's SetupActivity) ------------------------------------------

    private void setImportStatus(String text) {
        if (importStatus != null) importStatus.setText(text);
    }

    private void toast(String text) {
        Toast.makeText(this, text, Toast.LENGTH_LONG).show();
    }

    private LinearLayout column() {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        return column;
    }

    private View space(int heightDp) {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(heightDp)));
        return v;
    }

    private MaterialCardView card(View... views) {
        MaterialCardView card = new MaterialCardView(this);
        card.setCardBackgroundColor(CARD);
        card.setRadius(dp(16));
        card.setCardElevation(dp(4));
        card.setUseCompatPadding(true);
        card.setStrokeWidth(0);
        LinearLayout layout = column();
        layout.setPadding(dp(16), dp(12), dp(16), dp(16));
        for (View v : views) {
            if (v != null) layout.addView(v);
        }
        card.addView(layout);
        return card;
    }

    private LinearLayout section(String text) {
        LinearLayout box = column();
        box.setPadding(0, dp(4), 0, dp(10));
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setTextColor(Color.WHITE);
        box.addView(t);
        View line = new View(this);
        line.setBackgroundColor(ACCENT);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(2));
        lp.topMargin = dp(8);
        box.addView(line, lp);
        return box;
    }

    private TextView heading(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setTextColor(Color.WHITE);
        t.setPadding(0, dp(12), 0, 0);
        return t;
    }

    private TextView summary(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        t.setTextColor(GREY);
        return t;
    }

    // A setting the game does not apply yet: stored, and marked so under its description.
    private TextView notYetNote(String text) {
        TextView t = summary(text);
        t.setTextColor(NOT_YET);
        t.setPadding(0, dp(2), 0, 0);
        return t;
    }

    private View notYet(View setting) {
        if (setting instanceof LinearLayout) {
            LinearLayout box = (LinearLayout) setting;
            int at = Math.min(box.getChildCount(), 2);
            box.addView(notYetNote("Ainda sem efeito no Forza Horizon 2."), at);
        }
        return setting;
    }

    private void primaryButton(MaterialButton b) {
        b.setCornerRadius(dp(12));
        b.setBackgroundTintList(byEnabled(0xFF3A3A3A, ACCENT));
        b.setTextColor(byEnabled(0xFF808080, Color.WHITE));
    }

    private void secondaryButton(MaterialButton b) {
        b.setCornerRadius(dp(10));
        b.setBackgroundTintList(byEnabled(0xFF242424, 0xFF2C2C2C));
        b.setTextColor(byEnabled(0xFF6A6A6A, Color.WHITE));
    }

    private void dangerButton(MaterialButton b) {
        b.setCornerRadius(dp(10));
        b.setBackgroundTintList(byEnabled(0xFF242424, 0xFF4A1A1A));
        b.setTextColor(byEnabled(0xFF6A6A6A, 0xFFFF6666));
    }

    private static ColorStateList byEnabled(int disabled, int enabled) {
        return new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}}, new int[] {disabled, enabled});
    }

    private LinearLayout buttonRow(View first, View second) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(8), 0, 0);
        LinearLayout.LayoutParams firstParams =
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        firstParams.rightMargin = dp(8);
        row.addView(first, firstParams);
        row.addView(second, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        return row;
    }

    private interface OnFlag {
        void set(boolean value);
    }

    private interface OnChoice {
        void chosen(String value);
    }

    private View toggle(String title, String description, boolean value, OnFlag save) {
        LinearLayout row = column();
        row.setPadding(0, dp(10), 0, dp(6));
        MaterialSwitch s = new MaterialSwitch(this);
        s.setText(title);
        s.setTextColor(Color.WHITE);
        s.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        s.setThumbTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}},
                new int[] {Color.WHITE, 0xFF9E9E9E}));
        s.setTrackTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}},
                new int[] {ACCENT, 0xFF3A3A3A}));
        s.setChecked(value);
        s.setOnCheckedChangeListener((v, on) -> save.set(on));
        row.addView(s, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        row.addView(summary(description));
        return row;
    }

    private MaterialRadioButton radio(String text) {
        MaterialRadioButton b = new MaterialRadioButton(this);
        b.setId(View.generateViewId());
        b.setText(text);
        b.setTextColor(Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        b.setButtonTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}},
                new int[] {ACCENT, 0xFF9E9E9E}));
        return b;
    }

    private View selector(String key, String title, String description, String[] labels, String[] values,
                          boolean horizontal) {
        return selector(key, title, description, labels, values, horizontal, null);
    }

    // Radio buttons; in a row only when they fit, so the texts do not wrap into a tall row.
    private View selector(String key, String title, String description, String[] labels, String[] values,
                          boolean horizontal, OnChoice after) {
        LinearLayout box = column();
        box.setPadding(0, dp(10), 0, dp(10));
        box.addView(heading(title));
        box.addView(summary(description));
        int total = 0;
        int longest = 0;
        for (String label : labels) {
            total += label.length();
            longest = Math.max(longest, label.length());
        }
        RadioGroup group = new RadioGroup(this);
        group.setOrientation(horizontal && total <= 30 && longest <= 14 ? RadioGroup.HORIZONTAL : RadioGroup.VERTICAL);
        group.setPadding(0, dp(6), 0, 0);
        String current = GameOptions.get(this, key);
        for (int i = 0; i < labels.length; i++) {
            MaterialRadioButton b = radio(labels[i]);
            b.setTag(values[i]);
            group.addView(b);
            if (values[i].equals(current)) b.setChecked(true);
        }
        group.setOnCheckedChangeListener((g, id) -> {
            View b = g.findViewById(id);
            if (b != null && ((MaterialRadioButton) b).isChecked()) {
                GameOptions.set(this, key, (String) b.getTag());
                if (after != null) after.chosen((String) b.getTag());
            }
        });
        box.addView(group);
        return box;
    }

    // As selector(), in a dropdown menu: for the long lists.
    private View dropdown(String key, String title, String description, String[] labels, String[] values) {
        LinearLayout box = column();
        box.setPadding(0, dp(10), 0, dp(10));
        box.addView(heading(title));
        box.addView(summary(description));
        TextInputLayout field = new TextInputLayout(this, null,
                com.google.android.material.R.attr.textInputOutlinedExposedDropdownMenuStyle);
        field.setBoxStrokeColor(ACCENT);
        MaterialAutoCompleteTextView list = new MaterialAutoCompleteTextView(field.getContext());
        // Chosen from the list only: no keyboard, no free text.
        list.setInputType(InputType.TYPE_NULL);
        list.setTextColor(Color.WHITE);
        list.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, labels));
        int chosen = Math.max(0, Arrays.asList(values).indexOf(GameOptions.get(this, key)));
        list.setText(labels[chosen], false);
        list.setOnItemClickListener((parent, view, position, id) -> GameOptions.set(this, key, values[position]));
        field.addView(list, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        // After adding the field: the dropdown arrow needs its AutoCompleteTextView.
        field.setEndIconMode(TextInputLayout.END_ICON_DROPDOWN_MENU);
        field.setEndIconTintList(ColorStateList.valueOf(Color.WHITE));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        box.addView(field, lp);
        return box;
    }

    private static void setEnabledDeep(View view, boolean enabled) {
        view.setEnabled(enabled);
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) setEnabledDeep(group.getChildAt(i), enabled);
        }
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onDestroy() {
        worker.shutdown();
        super.onDestroy();
    }
}
