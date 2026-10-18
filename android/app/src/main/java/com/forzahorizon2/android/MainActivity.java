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
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.provider.Settings;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.StyleSpan;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.documentfile.provider.DocumentFile;

import static com.forzahorizon2.android.AppLanguage.tr;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.slider.Slider;
import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
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
    private static final int REQUEST_GPU_PROBE = 46;
    private static final int REQUEST_ISO = 47;
    private static final String TREE_URI = "tree_uri";
    // Relative to the internal storage root (the "jogos" folder holds the user's games).
    static final String GAME_FOLDER_NAME = "jogos/ForzaHorizon2";
    static final String PROJECT_URL = "https://github.com/Gabrick75/fh2-recompiled";

    private TextView importStatus;
    private LinearLayout checksList;
    private LinearLayout selectFolder;
    private LinearLayout selectIso;
    private LinearLayout stopIso;
    private MaterialButton launchGame;
    private LinearLayout saveReport;
    private LinearLayout rootReport;
    private LinearLayout rootProfile;
    private File pendingReport;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        AppLanguage.load(this);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        setContentView(buildScreen(state == null ? GRAPHICS : state.getInt(STATE_PAGE, GRAPHICS)));
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
    // The save import list: exports first, then the automatic backups, newest first.
    private void chooseSaveToImport() {
        List<File> saves = SaveTransfer.importable();
        if (saves.isEmpty()) {
            toast(tr("Nenhum save para importar em jogos/", "No save to import in jogos/") + GAME_FOLDER_NAME +
                    tr("-saves-exportados ou -saves.", "-saves-exportados or -saves."));
            return;
        }
        String[] names = new String[saves.size()];
        for (int i = 0; i < names.length; i++) {
            File f = saves.get(i);
            names[i] = f.getName() + (f.getParentFile().equals(SaveTransfer.exportsFolder()) ? tr(" · exportado", " · export")
                                                                                               : " · backup");
        }
        new android.app.AlertDialog.Builder(this)
                .setTitle(tr("Importar save", "Import a save"))
                .setItems(names, (dialog, which) -> new android.app.AlertDialog.Builder(this)
                        .setTitle(tr("Trocar o progresso atual?", "Replace the current progress?"))
                        .setMessage(tr("O save de " + names[which] + " substitui o atual. O atual continua no " +
                                        "backup automático da última vez que o jogo abriu.",
                                "The save from " + names[which] + " replaces the current one. The current one " +
                                        "stays in the automatic backup of the last time the game started."))
                        .setPositiveButton(tr("Importar", "Import"), (d, w) -> importSave(saves.get(which)))
                        .setNegativeButton(tr("Cancelar", "Cancel"), null)
                        .show())
                .show();
    }

    private void importSave(File source) {
        try {
            SaveTransfer.importFrom(this, source);
            toast(tr("Save importado de ", "Save imported from ") + source.getName());
        } catch (IOException e) {
            toast(tr("Não foi possível importar: ", "Could not import: ") + e.getMessage());
        }
    }

    private void startBenchIfAsked(Intent intent) {
        if (intent == null) return;
        // From adb: --es fh2.importar_save <folder> (SaveTransfer.resolve), before the bench if both are given.
        String save = intent.getStringExtra("fh2.importar_save");
        if (save != null) {
            intent.removeExtra("fh2.importar_save");
            importSave(SaveTransfer.resolve(save));
        }
        // From adb, to test drivers without the screen: --es fh2.importar_driver <zip> imports one and tests it
        // (chosen if it passes); --es fh2.escolher_driver <name> chooses one ("" = system) without a test. The
        // tests' results go to logcat (tag FH2).
        String driverZip = intent.getStringExtra("fh2.importar_driver");
        if (driverZip != null) {
            intent.removeExtra("fh2.importar_driver");
            importDriver(Uri.fromFile(new File(driverZip)));
        }
        String driverName = intent.getStringExtra("fh2.escolher_driver");
        if (driverName != null) {
            intent.removeExtra("fh2.escolher_driver");
            GameOptions.set(this, GameOptions.DRIVER, driverName);
            if (ui != null) rebuildPage(PERFORMANCE);
        }
        if (!intent.getBooleanExtra("fh2.banco", false)) return;
        Intent game = new Intent(this, GameActivity.class);
        String[] extra = intent.getStringArrayExtra("fh2.args");
        if (extra != null) game.putExtra("fh2.args", extra);
        String resolution = intent.getStringExtra("fh2.resolucao");  // 480p, 720p or native, for this run only
        if (resolution != null) game.putExtra("fh2.resolucao", resolution);
        if (intent.getBooleanExtra("fh2.uso", false)) game.putExtra("fh2.uso", true);  // the usage overlay, this run
        startActivity(game);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Back from the game: it may have unlocked some.
        if (content != null && pages[ACHIEVEMENTS] != null) rebuildPage(ACHIEVEMENTS);
        if (hasStorageAccess()) {
            showGameFolder(null);
        }
    }

    // ---- Screen --------------------------------------------------------------------------------------------
    // A sidebar with the pages, one under the other, and the Play button always at hand at its foot; on the right
    // the chosen page, in grouped lists (Menu). Graphics comes first and starts with the renderer: the options under
    // it are the chosen renderer's own, so the emulated and the native ones never mix. Every text is in the app's
    // two languages (AppLanguage.tr); changing the language builds the screen again.

    private static final int GRAPHICS = 0, DISPLAY = 1, PERFORMANCE = 2, SOUND = 3, ACHIEVEMENTS = 4, LANGUAGE = 5,
            GAME = 6, ABOUT = 7;
    private static final int[] PAGE_ICONS = {R.drawable.ic_menu_graficos, R.drawable.ic_menu_tela,
            R.drawable.ic_menu_desempenho, R.drawable.ic_menu_outros, R.drawable.ic_menu_conquistas,
            R.drawable.ic_menu_idioma, R.drawable.ic_menu_jogo, R.drawable.ic_menu_sobre};
    private static final String STATE_PAGE = "page";

    private Menu ui;
    private FrameLayout content;
    private final ScrollView[] pages = new ScrollView[PAGE_ICONS.length];
    private final LinearLayout[] navRows = new LinearLayout[PAGE_ICONS.length];
    private int currentPage = -1;
    private TextView playStatus;
    private View playDot;
    private boolean gameReady;
    private LinearLayout rendererOptions;
    private TextView rendererNote;
    private Boolean shadersFound;  // what the native renderer's requirement row shows; null before it is built

    private static String pageName(int page) {
        switch (page) {
            case GRAPHICS: return tr("Gráficos", "Graphics");
            case DISPLAY: return tr("Tela", "Display");
            case PERFORMANCE: return tr("Desempenho", "Performance");
            case SOUND: return tr("Outros", "Others");
            case ACHIEVEMENTS: return tr("Conquistas", "Achievements");
            case LANGUAGE: return tr("Idioma", "Language");
            case GAME: return tr("Jogo e saves", "Game and saves");
            default: return tr("Sobre", "About");
        }
    }

    private View buildScreen(int page) {
        ui = new Menu(this);
        currentPage = -1;
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.HORIZONTAL);
        root.setBackgroundColor(Menu.BACKGROUND);
        root.addView(sidebar(), new LinearLayout.LayoutParams(dp(244), LinearLayout.LayoutParams.MATCH_PARENT));
        View line = new View(this);
        line.setBackgroundColor(Menu.SEPARATOR);
        root.addView(line, new LinearLayout.LayoutParams(1, LinearLayout.LayoutParams.MATCH_PARENT));
        content = new FrameLayout(this);
        root.addView(content, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));
        for (int i = 0; i < pages.length; i++) pages[i] = buildPage(i);
        showPage(page, false);
        return root;
    }

    /** The app's language changed: the whole screen again, in it, on the same page. */
    private void rebuildScreen() {
        AppLanguage.load(this);
        int page = currentPage;
        setContentView(buildScreen(page));
        Menu.enter(content);
        if (hasStorageAccess()) showGameFolder(null);
        else setImportStatus(storageNotice());
    }

    private View sidebar() {
        LinearLayout bar = ui.column();
        bar.setBackgroundColor(Menu.SIDEBAR);
        bar.setPadding(dp(12), dp(14), dp(12), dp(12));

        // The name, and under it whether the game is ready to play.
        LinearLayout brand = new LinearLayout(this);
        brand.setOrientation(LinearLayout.HORIZONTAL);
        brand.setGravity(Gravity.CENTER_VERTICAL);
        brand.setPadding(dp(6), 0, dp(6), dp(12));
        brand.addView(appIcon(34), new LinearLayout.LayoutParams(dp(34), dp(34)));
        LinearLayout names = ui.column();
        names.setPadding(dp(10), 0, 0, 0);
        names.addView(ui.text("FH2 Recompiled", 16, Menu.PRIMARY, true));
        LinearLayout status = new LinearLayout(this);
        status.setOrientation(LinearLayout.HORIZONTAL);
        status.setGravity(Gravity.CENTER_VERTICAL);
        playDot = ui.dot(Menu.TERTIARY);
        LinearLayout.LayoutParams dotParams = new LinearLayout.LayoutParams(dp(7), dp(7));
        status.addView(playDot, dotParams);
        playStatus = ui.text("", 12, Menu.SECONDARY, false);
        playStatus.setPadding(dp(6), 0, 0, 0);
        playStatus.setSingleLine(true);
        status.addView(playStatus);
        names.addView(status);
        brand.addView(names);
        bar.addView(brand);

        LinearLayout nav = ui.column();
        for (int i = 0; i < navRows.length; i++) {
            navRows[i] = navRow(i);
            nav.addView(navRows[i], new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(40)));
        }
        ScrollView navScroll = new ScrollView(this);
        navScroll.setVerticalScrollBarEnabled(false);
        navScroll.setVerticalFadingEdgeEnabled(true);  // a short screen (or a big font) fades the list out, not cuts it
        navScroll.setFadingEdgeLength(dp(24));
        navScroll.addView(nav);
        bar.addView(navScroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        launchGame = new MaterialButton(this);
        launchGame.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        launchGame.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        launchGame.setAllCaps(false);
        launchGame.setCornerRadius(dp(14));
        launchGame.setInsetTop(0);
        launchGame.setInsetBottom(0);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            launchGame.setOutlineSpotShadowColor(Menu.ACCENT);  // a magenta glow instead of a grey shadow
            launchGame.setOutlineAmbientShadowColor(Menu.ACCENT);
        }
        launchGame.setOnClickListener(view -> {
            if (gameReady) play();
            else showPage(GAME, true);
        });
        Menu.pressable(launchGame);
        LinearLayout.LayoutParams playParams =
                new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(48));
        playParams.topMargin = dp(8);
        bar.addView(launchGame, playParams);
        setGameReady(false);
        return bar;
    }

    private ImageView appIcon(int sizeDp) {
        ImageView icon = new ImageView(this);
        icon.setImageResource(R.mipmap.ic_logo);
        icon.setScaleType(ImageView.ScaleType.CENTER_CROP);
        icon.setBackground(Menu.rounded(Menu.GROUP, dp(sizeDp * 0.24f)));
        icon.setClipToOutline(true);
        return icon;
    }

    private LinearLayout navRow(int page) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(10), 0, dp(10), 0);
        ImageView icon = new ImageView(this);
        icon.setImageResource(PAGE_ICONS[page]);
        row.addView(icon, new LinearLayout.LayoutParams(dp(20), dp(20)));
        TextView label = ui.text(pageName(page), 15, Menu.PRIMARY, false);
        label.setPadding(dp(12), 0, 0, 0);
        row.addView(label);
        row.setOnClickListener(v -> showPage(page, true));
        Menu.pressable(row);
        return row;
    }

    private void showPage(int page, boolean animate) {
        if (page == currentPage) return;
        currentPage = page;
        content.removeAllViews();
        content.addView(pages[page]);
        if (animate) Menu.enter(pages[page]);
        for (int i = 0; i < navRows.length; i++) {
            boolean chosen = i == page;
            LinearLayout row = navRows[i];
            row.setBackground(Menu.rounded(chosen ? Menu.ACCENT_SOFT : Color.TRANSPARENT, dp(10)));
            ((ImageView) row.getChildAt(0)).setImageTintList(
                    ColorStateList.valueOf(chosen ? Menu.ACCENT_TEXT : Menu.SECONDARY));
            TextView label = (TextView) row.getChildAt(1);
            label.setTextColor(chosen ? Menu.PRIMARY : 0xFFC7C7CC);
            label.setTypeface(Typeface.create("sans-serif-medium", chosen ? Typeface.BOLD : Typeface.NORMAL));
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putInt(STATE_PAGE, currentPage);
    }

    /** The Play button and the line under the name: Play when the game is in place, otherwise a way to it. */
    private void setGameReady(boolean ready) {
        gameReady = ready;
        launchGame.setText(ready ? tr("Jogar", "Play") : tr("Instalar o jogo", "Install the game"));
        launchGame.setBackgroundTintList(byEnabled(Menu.FILL, ready ? Menu.ACCENT : Menu.FILL));
        launchGame.setTextColor(byEnabled(Menu.TERTIARY, ready ? Color.WHITE : Menu.ACCENT_TEXT));
        launchGame.setElevation(ready ? dp(6) : 0);
        playStatus.setText(ready ? tr("Pronto para jogar", "Ready to play") : tr("Jogo não encontrado", "Game not found"));
        ((android.graphics.drawable.GradientDrawable) playDot.getBackground()).setColor(ready ? Menu.GREEN : Menu.AMBER);
    }

    private ScrollView buildPage(int page) {
        switch (page) {
            case GRAPHICS: return graphicsPage();
            case DISPLAY: return displayPage();
            case PERFORMANCE: return performancePage();
            case SOUND: return soundPage();
            case ACHIEVEMENTS: return achievementsPage();
            case LANGUAGE: return languagePage();
            case GAME: return gamePage();
            default: return aboutPage();
        }
    }

    /** Builds a page again (after a change that alters its rows), at the same scroll position. */
    private void rebuildPage(int page) {
        int y = pages[page].getScrollY();
        ScrollView fresh = buildPage(page);
        pages[page] = fresh;
        if (currentPage == page) {
            content.removeAllViews();
            content.addView(fresh);
        }
        fresh.post(() -> fresh.scrollTo(0, y));
    }

    /** A page's scrolling column, at most 680 dp wide (on a tablet the groups stay readable). */
    private ScrollView pageScroll(View... parts) {
        LinearLayout column = ui.column();
        column.setPadding(dp(28), dp(24), dp(28), dp(32));
        for (View part : parts) if (part != null) column.addView(part);
        FrameLayout holder = new FrameLayout(this);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER_HORIZONTAL);
        holder.addView(column, lp);
        holder.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            int width = Math.min(r - l, dp(680));
            if (column.getLayoutParams().width != width) {
                column.getLayoutParams().width = width;
                column.post(column::requestLayout);
            }
        });
        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.setClipToPadding(false);
        scroll.addView(holder);
        return scroll;
    }

    // ---- Graphics: the renderer first, then its own options ------------------------------------------------

    private ScrollView graphicsPage() {
        boolean nativeRenderer = GameOptions.nativeRenderer(this);
        SegmentedControl renderer = new SegmentedControl(this,
                new String[] {tr("Emulado", "Emulated"), tr("Nativo", "Native")});
        renderer.choose(nativeRenderer ? 1 : 0, false);
        rendererNote = ui.footer("");
        rendererOptions = ui.column();
        fillRendererOptions(nativeRenderer);
        if (emulatedUnsupported == null) {
            worker.execute(() -> {
                boolean unsupported = emulatedRendererUnsupported(this);
                mainHandler.post(() -> {
                    emulatedUnsupported = unsupported;
                    if (!isFinishing() && !isDestroyed()) {
                        fillRendererNote(GameOptions.nativeRenderer(this));
                    }
                });
            });
        }
        renderer.setOnChoice(index -> {
            GameOptions.set(this, GameOptions.RENDERER, index == 1 ? "native" : "emulated");
            fillRendererOptions(index == 1);
            Menu.enter(rendererOptions);
        });
        return pageScroll(ui.pageTitle(pageName(GRAPHICS),
                        tr("Escolha o renderizador: as opções abaixo são as dele.",
                                "Pick the renderer: the options below are its own.")),
                ui.header(tr("Renderizador", "Renderer")),
                ui.group(ui.stackedRow(tr("Como o jogo é desenhado", "How the game is drawn"), null, renderer)),
                rendererNote,
                rendererOptions);
    }

    private void fillRendererOptions(boolean nativeRenderer) {
        fillRendererNote(nativeRenderer);
        rendererOptions.removeAllViews();
        fillRendererOptionsBody(nativeRenderer);
    }

    // null until the background check (emulatedRendererUnsupported) has answered.
    private Boolean emulatedUnsupported;

    private void fillRendererNote(boolean nativeRenderer) {
        String note = nativeRenderer
                ? tr("Nativo (experimental): desenha os comandos do jogo direto em Vulkan, com os shaders traduzidos. " +
                     "Bem mais rápido, mas ainda tem erros de imagem.",
                     "Native (experimental): draws the game's commands straight in Vulkan, with the translated " +
                     "shaders. Much faster, but it still has image errors.")
                : tr("Emulado: a GPU do Xbox 360 emulada. É o modo testado e o mais fiel à imagem do console.",
                     "Emulated: the Xbox 360 GPU, emulated. The tested mode, and the most faithful to the console's " +
                     "image.");
        if (!nativeRenderer && Boolean.TRUE.equals(emulatedUnsupported)) {
            note += "\n\n" + emulatedUnsupportedMessage();
        }
        rendererNote.setText(note);
    }

    private void fillRendererOptionsBody(boolean nativeRenderer) {
        shadersFound = null;
        fsrModeRefreshers.clear();
        if (nativeRenderer) {
            boolean library = shaderLibrary(this).isFile();
            shadersFound = library;
            rendererOptions.addView(ui.header(tr("Requisito", "Requirement")));
            rendererOptions.addView(ui.group(ui.statusRow(tr("Biblioteca de shaders", "Shader library"),
                    tr("fh2_shaders.nfsp, junto dos arquivos do jogo (" + GAME_FOLDER_NAME + "). Sem ela o nativo " +
                       "não tem os shaders traduzidos.",
                       "fh2_shaders.nfsp, next to the game files (" + GAME_FOLDER_NAME + "). Without it the native " +
                       "renderer has no translated shaders."),
                    library, library ? tr("Encontrada", "Found") : tr("Faltando", "Missing"))));

            rendererOptions.addView(ui.header(tr("Imagem do nativo", "Native image")));
            SegmentedControl aa = antiAliasingControl(null);
            rendererOptions.addView(ui.group(
                    renderScaleRow(true, null),
                    ui.stackedRow(tr("Suavização de bordas", "Anti-aliasing"),
                            tr("O FXAA tira o serrilhado com um filtro barato sobre a imagem final.",
                               "FXAA smooths jagged edges with a cheap filter over the final image."), aa),
                    anisotropicRow(),
                    shadowMapRow(),
                    scalingRow(),
                    fsrModeRow(),
                    sharpnessRow()));
            rendererOptions.addView(ui.footer(tr("MSAA 2x/4x desenha a cena 3D com 2/4 amostras e resolve no passe " +
                    "(custa GPU); o FXAA é um filtro barato sobre a imagem final.",
                    "MSAA 2x/4x draws the 3D scene with 2/4 samples and resolves in the pass (costs GPU); FXAA " +
                    "is a cheap filter over the final image.")));

            rendererOptions.addView(ui.header(tr("Desempenho do nativo", "Native performance")));
            rendererOptions.addView(ui.group(fpsLimitRow(), onePassRow(
                    tr("Sem MSAA a cena cabe inteira e é desenhada uma vez só, em vez de em 3 faixas: menos trabalho " +
                       "para a CPU (+25 %).",
                       "Without MSAA the scene fits whole and is drawn once, not in 3 tiles: less work for the CPU " +
                       "(+25 %)."),
                    // B19: in 3 tiles the native renderer only shows the bottom one (black and grey bands above).
                    tr("Desligada, quebra o jogo: só a faixa de baixo da tela aparece, com faixas preta e cinza " +
                       "em cima.",
                       "Turned off, it breaks the game: only the bottom band of the screen shows, with black and " +
                       "grey bands above.")),
                    ui.toggleRow(tr("Cópia adiada da cena", "Deferred scene copy"),
                            tr("Experimental. A cópia da cena pronta (antes do HUD) só é feita se algo for usá-la, " +
                               "como as gotas de chuva; se o jogo não a usar, a GPU economiza a cópia. Desliga " +
                               "sozinha se notar diferença. Ganho ainda não medido.",
                               "Experimental. The copy of the finished scene (before the HUD) is only made if " +
                               "something will use it, like the raindrops; if the game does not, the GPU saves the " +
                               "copy. It turns itself off if it sees a difference. Gain not measured yet."),
                            GameOptions.flag(this, GameOptions.LAZY_COMPOSITE),
                            v -> GameOptions.flag(this, GameOptions.LAZY_COMPOSITE, v))));
        } else {
            rendererOptions.addView(ui.header(tr("Imagem do emulado", "Emulated image")));
            LinearLayout image = ui.group();
            Runnable[] refreshScale = new Runnable[1];
            LinearLayout fxaa = ui.choiceRow(GameOptions.FXAA_QUALITY, tr("Qualidade do FXAA", "FXAA quality"),
                    tr("Alta suaviza mais bordas, inclusive as fracas, e deixa a imagem um pouco mais suave.",
                       "High smooths more edges, the faint ones too, and softens the image a little."),
                    new String[] {"Normal", tr("Alta", "High")}, new String[] {"normal", "high"});
            Runnable showModes = () -> {
                fxaa.setVisibility("fxaa".equals(GameOptions.get(this, GameOptions.ANTI_ALIASING))
                        ? View.VISIBLE : View.GONE);
                Menu.refreshDividers(image);
            };
            image.addView(renderScaleRow(false, refreshScale));
            image.addView(ui.stackedRow(tr("Suavização de bordas", "Anti-aliasing"),
                    tr("Tira o serrilhado. O MSAA é o do jogo e o mais caro para a GPU (desligado rende +91 %; o 2x " +
                       "custa menos que o 4x, +37 %); o FXAA é um filtro barato sobre a imagem final.",
                       "Smooths jagged edges. MSAA is the game's own and the costliest for the GPU (off gives " +
                       "+91 %; 2x costs less than 4x, +37 %); FXAA is a cheap filter over the final image."),
                    antiAliasingControl(() -> {
                        showModes.run();
                        if ("fxaa".equals(GameOptions.get(this, GameOptions.ANTI_ALIASING))) Menu.enter(fxaa);
                        refreshScale[0].run();
                    })));
            image.addView(fxaa);
            image.addView(anisotropicRow());
            image.addView(scalingRow());
            image.addView(fsrModeRow());
            image.addView(sharpnessRow());
            showModes.run();
            rendererOptions.addView(image);

            rendererOptions.addView(ui.header(tr("Desempenho do emulado", "Emulated performance")));
            rendererOptions.addView(ui.group(
                    fpsLimitRow(),
                    onePassRow(tr("O jogo desenha a cena em 3 faixas por causa do MSAA; sem ele a cena cabe inteira " +
                            "e é desenhada uma vez só: menos trabalho para a CPU (+25 %). Só vale com a suavização " +
                            "desligada ou FXAA.",
                            "The game draws the scene in 3 tiles because of MSAA; without it the scene fits whole and " +
                            "is drawn once: less work for the CPU (+25 %). Only with anti-aliasing off or FXAA.")),
                    ui.toggleRow(tr("Cópias diretas para texturas", "Direct copies to textures"),
                            tr("Experimental. Quando o jogo copia uma imagem que desenhou para usá-la como textura, " +
                               "copia direto na GPU, sem passar pela memória do console emulado. Desligue se " +
                               "aparecer alguma imagem errada.",
                               "Experimental. When the game copies an image it drew to use it as a texture, the copy " +
                               "stays on the GPU, without going through the emulated console's memory. Turn it off " +
                               "if an image looks wrong."),
                            GameOptions.flag(this, GameOptions.RESOLVE_SHORTCUT),
                            v -> GameOptions.flag(this, GameOptions.RESOLVE_SHORTCUT, v)),
                    ui.toggleRow(tr("Compilar shaders em segundo plano", "Compile shaders in the background"),
                            tr("Desligado: o jogo espera cada shader novo ficar pronto, e pode dar um tranco. " +
                               "Ligado: segue sem esperar, com menos trancos, mas o objeto daquele shader pode " +
                               "sumir ou piscar por um instante.",
                               "Off: the game waits for each new shader to be ready, which can stutter. On: it goes " +
                               "on without waiting, with less stutter, but that shader's object may vanish or " +
                               "flicker for a moment."),
                            GameOptions.flag(this, GameOptions.ASYNC_SHADERS),
                            v -> GameOptions.flag(this, GameOptions.ASYNC_SHADERS, v))));
        }
            rendererOptions.addView(ui.header(tr("Ainda sem efeito", "Not applied yet")));
            rendererOptions.addView(ui.group(
                    ui.choiceRow(GameOptions.SHADOWS, tr("Sombras", "Shadows"),
                        tr("A cada quanto os mapas de sombras são redesenhados. A cada 2 quadros custa menos e quase " +
                           "não se nota.",
                           "How often the shadow maps are redrawn. Every 2 frames costs less and is hardly noticed."),
                        new String[] {tr("Todo quadro", "Every frame"), tr("A cada 2", "Every 2")},
                        new String[] {"1", "2"}),
                ui.choiceRow(GameOptions.SHADOW_DISTANCE, tr("Distância das sombras", "Shadow distance"),
                        tr("Até onde os objetos projetam sombra: é aí que está quase todo o custo delas. Longe é " +
                           "como o jogo.",
                           "How far objects cast shadows: nearly all of their cost is there. Far is the game's own."),
                        new String[] {tr("Longe", "Far"), tr("Média", "Medium"), tr("Perto", "Near")},
                        new String[] {"far", "medium", "near"}),
                ui.choiceRow(GameOptions.CAR_REFLECTIONS, tr("Reflexos do carro", "Car reflections"),
                        tr("Faces do mapa de reflexos do carro redesenhadas a cada quadro.",
                           "Faces of the car's reflection map redrawn every frame."),
                        new String[] {tr("Altos", "High"), tr("Médios", "Medium"), tr("Baixos", "Low")},
                        new String[] {"high", "medium", "low"})));
        rendererOptions.addView(ui.footer(tr("Ficam guardadas, mas o jogo ainda não as aplica.",
                "They are stored, but the game does not apply them yet.")));
    }

    // The native renderer's translated shaders (nfsmw_nativo_sistema.cpp also looks next to the executable).
    // With the game read from an ISO: next to the ISO, else in the game folder (fh2_nativo_biblioteca_dir).
    static File shaderLibrary(android.content.Context context) {
        File iso = GameIso.chosen(context);
        if (iso != null) {
            File junto = new File(iso.getParentFile(), "fh2_shaders.nfsp");
            if (junto.isFile()) return junto;
        }
        return new File(gameRoot(), "fh2_shaders.nfsp");
    }

    // The game's FPS limit, through the emulated vblank rate (GameOptions.vblankRate). Both renderers.
    private LinearLayout fpsLimitRow() {
        return ui.choiceRow(GameOptions.FPS, tr("Limite de FPS", "FPS limit"),
                tr("O teto do jogo; 30 é o do Xbox 360. Acima dele o jogo mostra mais quadros se o aparelho der " +
                   "conta, e a tela troca para a taxa mais alta que tiver. Telas de início e carregamentos podem ir " +
                   "ao dobro. Mais quadros esquentam mais.",
                   "The game's cap; 30 is the Xbox 360's. Above it the game shows more frames if the phone keeps " +
                   "up, and the screen switches to its highest rate. Start screens and loadings may reach twice " +
                   "the cap. More frames mean more heat."),
                new String[] {"30", "60", "90", "120", tr("Livre", "Free")},
                new String[] {"30", "60", "90", "120", "0"});
    }

    /** The native renderer's shadow map size: 0 (no shadows) to 100 % of the game's 1600x1600, in 5 % steps. */
    private LinearLayout shadowMapRow() {
        LinearLayout box = ui.column();
        LinearLayout readout = new LinearLayout(this);
        readout.setOrientation(LinearLayout.HORIZONTAL);
        readout.setGravity(Gravity.BOTTOM);
        TextView percent = ui.text("", 22, Menu.PRIMARY, true);
        TextView size = ui.text("", 13, Menu.SECONDARY, false);
        size.setPadding(dp(10), 0, 0, dp(3));
        readout.addView(percent);
        readout.addView(size);
        box.addView(readout);
        Slider slider = ui.slider(0, 100, 5, Math.round(GameOptions.shadowMapScalePercent(this) / 5f) * 5);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(2);
        box.addView(slider, lp);
        Runnable show = () -> {
            int value = GameOptions.shadowMapScalePercent(this);
            if (value == 0) {
                percent.setText(tr("Desligadas", "Off"));
                size.setText("");
                return;
            }
            // Same rounding as Fh2Escalar (nfsmw_nativo_destinos.cpp): the 1040 target in multiples of 16, at least
            // 64; the 1024x1024 the game resolves scales with it.
            int side = value >= 100 ? 1024 : Math.max(64, ((1040 * value / 100) + 15) & ~15) * 1024 / 1040;
            percent.setText(value + " %");
            size.setText(side + "×" + side + (value >= 100 ? tr(" · como o Xbox 360", " · as on the Xbox 360") : ""));
        };
        slider.addOnChangeListener((s, value, fromUser) -> {
            if (!fromUser) return;
            GameOptions.set(this, GameOptions.SHADOW_MAP_SCALE, String.valueOf(Math.round(value)));
            show.run();
        });
        show.run();
        return ui.stackedRow(tr("Qualidade das sombras", "Shadow quality"),
                tr("Tamanho dos mapas de sombras do sol (o jogo usa 4 de 1024×1024). Menor deixa as bordas das " +
                   "sombras mais borradas e alivia a GPU; 0 desliga as sombras. Vale ao abrir o jogo.",
                   "Size of the sun's shadow maps (the game uses four of 1024×1024). Smaller makes shadow edges " +
                   "blurrier and eases the GPU; 0 turns shadows off. Applied when the game starts."), box);
    }

    private LinearLayout anisotropicRow() {
        return ui.choiceRow(GameOptions.ANISOTROPIC, tr("Filtro anisotrópico", "Anisotropic filtering"),
                tr("Texturas mais nítidas vistas em ângulo, como a estrada ao longe.",
                   "Sharper textures seen at an angle, like the road in the distance."),
                new String[] {tr("Não", "Off"), "2x", "4x", "16x"}, new String[] {"0", "2", "3", "5"});
    }

    private LinearLayout onePassRow(String description) {
        return onePassRow(description, null);
    }

    /** With a warning, it follows the description in bold. */
    private LinearLayout onePassRow(String description, String warning) {
        LinearLayout row = ui.toggleRow(tr("Cena em uma passada", "Scene in one pass"), description,
                GameOptions.flag(this, GameOptions.ONE_PASS), v -> GameOptions.flag(this, GameOptions.ONE_PASS, v));
        if (warning != null) {
            SpannableStringBuilder text = new SpannableStringBuilder(description).append(' ');
            int start = text.length();
            text.append(warning);
            text.setSpan(new StyleSpan(Typeface.BOLD), start, text.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            Menu.descriptionOf(row).setText(text);
        }
        return row;
    }

    /**
     * Render resolution, as a percentage of the Xbox 360's 1280x720, with the size it really becomes. Emulated: in
     * eighths (12.5 % steps, quarters with MSAA 4x); native: 12 (160x90) to 100, any value. refreshOut gets the refresh, for
     * the anti-aliasing choice that changes the emulated rounding.
     */
    private final java.util.List<Runnable> fsrModeRefreshers = new java.util.ArrayList<>();

    private void refreshFsrModeRows() {
        for (Runnable refresh : fsrModeRefreshers) refresh.run();
    }

    /**
     * Off / FXAA / MSAA 2x / MSAA 4x, the same control for both renderers. Stored as "msaa" + MSAA_SAMPLES (the
     * emulated renderer's form), so switching renderers keeps the choice. onChange (may be null) runs after a
     * new choice is stored.
     */
    private SegmentedControl antiAliasingControl(Runnable onChange) {
        SegmentedControl aa = new SegmentedControl(this, new String[] {tr("Desligada", "Off"), "FXAA",
                "MSAA 2x", "MSAA 4x"});
        String value = GameOptions.get(this, GameOptions.ANTI_ALIASING);
        boolean msaa2 = "2".equals(GameOptions.get(this, GameOptions.MSAA_SAMPLES));
        aa.choose("fxaa".equals(value) ? 1 : "msaa".equals(value) ? (msaa2 ? 2 : 3) : 0, false);
        aa.setOnChoice(index -> {
            GameOptions.set(this, GameOptions.ANTI_ALIASING, index == 1 ? "fxaa" : index >= 2 ? "msaa" : "off");
            if (index >= 2) GameOptions.set(this, GameOptions.MSAA_SAMPLES, index == 2 ? "2" : "4");
            if (onChange != null) onChange.run();
        });
        return aa;
    }

    private View scalingRow() {
        return ui.choiceRow(GameOptions.SCALING, tr("Escala para a tela", "Scaling to the screen"),
                tr("Como a imagem é levada ao tamanho da tela. O FSR é mais nítido quando o jogo desenha " +
                   "abaixo da resolução da tela; o CAS só aumenta a nitidez.",
                   "How the image is brought to the screen's size. FSR is sharper when the game draws below " +
                   "the screen's resolution; CAS only sharpens."),
                new String[] {"Bilinear", "FSR 1.0", "CAS"}, new String[] {"bilinear", "fsr", "cas"},
                value -> {
                    // Leaving FSR must also leave its mode: a stored preset must never override the scale.
                    if (!"fsr".equals(value)) GameOptions.set(this, GameOptions.FSR_MODE, "off");
                    refreshFsrModeRows();
                });
    }

    private View fsrModeRow() {
        LinearLayout row = ui.choiceRow(GameOptions.FSR_MODE, tr("Modo FSR", "FSR mode"),
                tr("Atalho: escala de render + FSR 1.0 juntos (77/67/59/50/33 % de 1280×720). Desligado vale " +
                   "a escala e o efeito escolhidos acima.",
                   "Shortcut: render scale + FSR 1.0 together (77/67/59/50/33 % of 1280x720). Off keeps the " +
                   "scale and effect chosen above."),
                new String[] {tr("Desligado", "Off"), tr("Ultra qualidade", "Ultra quality"),
                        tr("Qualidade", "Quality"), tr("Equilibrado", "Balanced"), tr("Desempenho", "Performance"),
                        tr("Ultra desempenho", "Ultra performance")},
                new String[] {"off", "ultra_quality", "quality", "balanced", "performance", "ultra_performance"});
        Runnable refresh = () -> row.setVisibility(
                "fsr".equals(GameOptions.get(this, GameOptions.SCALING)) ? View.VISIBLE : View.GONE);
        fsrModeRefreshers.add(refresh);
        refresh.run();
        return row;
    }

    private View sharpnessRow() {
        LinearLayout box = ui.column();
        TextView readout = ui.text("", 22, Menu.PRIMARY, true);
        box.addView(readout);
        int stored;
        try {
            stored = Math.max(0, Math.min(100, Integer.parseInt(GameOptions.get(this, GameOptions.SHARPNESS).trim())));
        } catch (NumberFormatException e) {
            stored = 0;
        }
        Slider slider = ui.slider(0, 100, 5, stored / 5 * 5);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(2);
        box.addView(slider, lp);
        Runnable show = () -> {
            int value;
            try {
                value = Math.max(0, Math.min(100,
                        Integer.parseInt(GameOptions.get(this, GameOptions.SHARPNESS).trim())));
            } catch (NumberFormatException e) {
                value = 0;
            }
            readout.setText(value + " %");
        };
        slider.addOnChangeListener((s, value, fromUser) -> {
            if (!fromUser) return;
            GameOptions.set(this, GameOptions.SHARPNESS, String.valueOf(Math.round(value / 5f) * 5));
            show.run();
        });
        show.run();
        return ui.stackedRow(tr("Nitidez extra (CAS/FSR)", "Extra sharpness (CAS/FSR)"),
                tr("Nitidez a mais sobre o CAS ou o FSR: 0 mantém o padrão. Vale na próxima vez que o jogo abrir.",
                   "Extra sharpness on top of CAS or FSR: 0 keeps the default. Takes effect the next time the game starts."),
                box);
    }

    private LinearLayout renderScaleRow(boolean nativeRenderer, Runnable[] refreshOut) {
        LinearLayout box = ui.column();
        LinearLayout readout = new LinearLayout(this);
        readout.setOrientation(LinearLayout.HORIZONTAL);
        readout.setGravity(Gravity.BOTTOM);
        TextView percent = ui.text("", 22, Menu.PRIMARY, true);
        TextView size = ui.text("", 13, Menu.SECONDARY, false);
        size.setPadding(dp(10), 0, 0, dp(3));
        readout.addView(percent);
        readout.addView(size);
        box.addView(readout);
        Slider slider;
        if (nativeRenderer) {
            slider = ui.slider(12, 100, 1, GameOptions.nativeRenderScalePercent(this));
        } else {
            slider = ui.slider(1, 8, 1, GameOptions.renderScaleEighths(GameOptions.renderScalePercent(this), false));
        }
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(2);
        box.addView(slider, lp);

        String original = tr(" · a resolução original", " · the original resolution");
        String comma = tr(",", ".");
        Runnable show = () -> {
            if (nativeRenderer) {
                // Same rounding as Fh2Escalar (nfsmw_nativo_destinos.cpp): the width in multiples of 16.
                int value = GameOptions.nativeRenderScalePercent(this);
                int width = value >= 100 ? 1280 : ((1280 * value / 100) + 15) & ~15;
                percent.setText(value + " %");
                size.setText(width + "×" + (width * 720 / 1280) + (value >= 100 ? original : ""));
                return;
            }
            int eighths = GameOptions.renderScaleEighths(GameOptions.renderScalePercent(this), GameOptions.msaa4x(this));
            percent.setText((eighths % 2 == 0 ? String.valueOf(eighths * 25 / 2) : (eighths * 25 / 2) + comma + "5")
                    + " %");
            size.setText((1280 * eighths / 8) + "×" + (720 * eighths / 8) + (eighths == 8 ? original : ""));
        };
        slider.addOnChangeListener((s, value, fromUser) -> {
            if (!fromUser) return;
            // Emulated: the eighth stored as the smallest whole percent that rounds back to it (13, 25, 38...).
            int stored = nativeRenderer ? Math.round(value) : (Math.round(value) * 25 + 1) / 2;
            GameOptions.set(this, GameOptions.RENDER_SCALE, String.valueOf(stored));
            if (!nativeRenderer) s.performHapticFeedback(android.view.HapticFeedbackConstants.CLOCK_TICK);
            show.run();
        });
        show.run();
        if (refreshOut != null) refreshOut[0] = show;
        return ui.stackedRow(tr("Resolução de renderização", "Render resolution"), nativeRenderer
                ? tr("Quanto da resolução do Xbox 360 (1280×720) o jogo desenha: a cena e o pós-processamento (o HUD " +
                     "também fica menos nítido). Abaixo de 100 % a GPU trabalha bem menos.",
                     "How much of the Xbox 360's resolution (1280×720) the game draws: the scene and the " +
                     "post-processing (the HUD gets less sharp too). Below 100 % the GPU works much less.")
                : tr("Quanto da resolução do Xbox 360 (1280×720) o jogo desenha, em passos de 12,5 %. Abaixo de " +
                     "100 % a imagem fica menos nítida e a GPU trabalha bem menos.",
                     "How much of the Xbox 360's resolution (1280×720) the game draws, in 12.5 % steps. Below " +
                     "100 % the image is less sharp and the GPU works much less."), box);
    }

    // ---- Display, performance, sound -----------------------------------------------------------------------

    private ScrollView displayPage() {
        return pageScroll(ui.pageTitle(pageName(DISPLAY),
                        tr("A imagem final e o que aparece por cima dela.", "The final image and what shows over it.")),
                ui.header(tr("Imagem", "Image")),
                ui.group(resolutionRow(),
                        ui.toggleRow(tr("Esticar para a tela inteira", "Stretch to the whole screen"),
                                tr("Desligado: a imagem 16:9 do Xbox 360, com barras pretas. Ligado: preenche a " +
                                   "tela, mas distorce a imagem.",
                                   "Off: the Xbox 360's 16:9 image, with black bars. On: fills the screen, but " +
                                   "distorts the image."),
                                GameOptions.stretch(this), v -> GameOptions.stretch(this, v))),
                ui.header(tr("Apresentação", "Presentation")),
                ui.group(ui.choiceRow(GameOptions.PRESENT_MODE, tr("Modo de apresentação", "Present mode"),
                                tr("FIFO espera o vsync da tela: é o mais rápido (+57 % no menu) e esquenta menos. " +
                                   "MAILBOX repinta sem parar: um pouco menos de atraso, mas tira CPU e GPU do jogo.",
                                   "FIFO waits for the screen's vsync: the fastest (+57 % in the menu), and it runs " +
                                   "cooler. MAILBOX repaints nonstop: a little less lag, but it takes CPU and GPU " +
                                   "from the game."),
                                new String[] {"FIFO · vsync", "MAILBOX"}, new String[] {"fifo", "mailbox"}),
                        ui.toggleRow(tr("Vblank adaptativo", "Adaptive vblank"),
                                tr("Um quadro que perde o vsync por pouco é mostrado na hora, em vez de esperar o " +
                                   "próximo. Não muda o fps; desligue se notar trepidação.",
                                   "A frame that misses the vsync by a little is shown at once, instead of waiting " +
                                   "for the next one. It does not change the fps; turn it off if you notice judder."),
                                GameOptions.flag(this, GameOptions.ADAPTIVE_VBLANK),
                                v -> GameOptions.flag(this, GameOptions.ADAPTIVE_VBLANK, v))),
                ui.header(tr("Sobreposição", "Overlay")),
                ui.group(ui.toggleRow(tr("Mostrar FPS", "Show FPS"),
                                tr("Contador no canto: quadros por segundo e milissegundos por quadro.",
                                   "A counter in the corner: frames per second and milliseconds per frame."),
                                GameOptions.flag(this, GameOptions.SHOW_FPS),
                                v -> GameOptions.flag(this, GameOptions.SHOW_FPS, v)),
                        ui.toggleRow(tr("Mostrar uso de GPU, CPU e RAM", "Show GPU, CPU and RAM use"),
                                tr("Abaixo do FPS: ocupação da GPU; CPU do jogo (de todos os núcleos) e a thread " +
                                   "mais ocupada (perto de 100 % é ela que limita o FPS); memória do jogo e a livre " +
                                   "do aparelho, em GB.",
                                   "Under the FPS: GPU load; the game's CPU (of all cores) and the busiest thread " +
                                   "(near 100 % it is what limits the FPS); the game's memory and the phone's free " +
                                   "memory, in GB."),
                                GameOptions.flag(this, GameOptions.SHOW_USAGE),
                                v -> GameOptions.flag(this, GameOptions.SHOW_USAGE, v))));
    }

    // Display resolution: 480p and 720p only when the screen is bigger than them, plus the native one.
    private LinearLayout resolutionRow() {
        DisplayMetrics metrics = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
        int longSide = Math.max(metrics.widthPixels, metrics.heightPixels);
        int shortSide = Math.min(metrics.widthPixels, metrics.heightPixels);
        List<String> labels = new ArrayList<>();
        List<String> values = new ArrayList<>();
        if (shortSide > 480) {
            labels.add("480p");
            values.add(GameOptions.RES_480P);
        }
        if (shortSide > 720) {
            labels.add("720p");
            values.add(GameOptions.RES_720P);
        }
        labels.add(tr("Nativa", "Native"));
        values.add(GameOptions.RES_NATIVE);
        return ui.choiceRow(GameOptions.RESOLUTION, tr("Resolução", "Resolution"),
                tr("Tamanho da imagem final na tela (a nativa é " + longSide + "×" + shortSide + "). O jogo é " +
                   "renderizado do mesmo jeito.",
                   "Size of the final image on the screen (the native one is " + longSide + "×" + shortSide + "). " +
                   "The game renders the same way."),
                labels.toArray(new String[0]), values.toArray(new String[0]));
    }

    private ScrollView performancePage() {
        boolean adreno = GpuDrivers.isAdreno(this);
        return pageScroll(ui.pageTitle(pageName(PERFORMANCE),
                        tr("O processador e o driver da GPU.", "The processor and the GPU driver.")),
                ui.header(tr("Processador", "Processor")),
                ui.group(processorRows()),
                ui.header(tr("Driver da GPU", "GPU driver")),
                driverGroup(),
                ui.footer(adreno
                        ? tr("Importe o .zip de um driver para Android (Turnip, o driver livre do Mesa para Adreno, " +
                             "ou um da Qualcomm, com meta.json). Antes de escolher um driver o app testa se ele abre " +
                             "o Vulkan desta GPU. Vale na próxima vez que o jogo abrir.",
                             "Import an Android driver's .zip (Turnip, Mesa's free driver for Adreno, or a Qualcomm " +
                             "one, with meta.json). Before a driver is chosen the app tests whether it opens Vulkan " +
                             "on this GPU. Takes effect the next time the game starts.")
                        : tr("Os drivers Turnip e da Qualcomm são só para GPUs Adreno: com esta GPU, use o driver do " +
                             "sistema.",
                             "Turnip and Qualcomm drivers are for Adreno GPUs only: with this GPU, use the system " +
                             "driver.")),
                ui.header(tr("Teste do Vulkan", "Vulkan test")),
                probeGroup(),
                ui.footer(tr("Abre o driver como o jogo abre, cria um dispositivo Vulkan e confere o que os " +
                             "renderizadores precisam. Roda num processo à parte: um driver que trave não leva o app " +
                             "junto.",
                             "Opens the driver the way the game does, creates a Vulkan device and checks what the " +
                             "renderers need. It runs in a separate process: a driver that crashes does not take the " +
                             "app with it.")),
                ui.header(tr("Ainda sem efeito", "Not applied yet")),
                turboGroup());
    }

    private String chosenDriver() {
        String chosen = GameOptions.get(this, GameOptions.DRIVER);
        return GpuDrivers.find(this, chosen) == null ? "" : chosen;
    }

    private static String driverTitle(String name) {
        return name.isEmpty() ? tr("Driver do sistema", "System driver") : name;
    }

    // The system driver or an imported one; tapping one tests it first (testDriver). Import, test, delete.
    private View[] processorRows() {
        List<View> rows = new ArrayList<>();
        rows.add(ui.toggleRow(tr("Fixar threads nos núcleos", "Pin threads to cores"),
                tr("Deixa o jogo só nos núcleos rápidos do processador e o processador de comandos da GPU " +
                   "(o do emulado ou o do nativo) sozinho no mais rápido (+16 %).",
                   "Keeps the game on the processor's fast cores only, and the GPU command processor (the " +
                   "emulated or the native one) alone on the fastest (+16 %)."),
                GameOptions.flag(this, GameOptions.PIN_THREADS),
                v -> GameOptions.flag(this, GameOptions.PIN_THREADS, v)));
        rows.add(ui.toggleRow(tr("Núcleo mais rápido para o jogo", "Fastest core for the game"),
                tr("Em vez do anel, dá o núcleo mais rápido à thread mais ocupada do jogo (ela muda nos " +
                   "carregamentos, e o modo a segue). Para paisagens pesadas, onde a thread do jogo satura " +
                   "antes do anel; desligado vale o modo acima.",
                   "Instead of the ring, gives the fastest core to the game's busiest thread (it changes on " +
                   "loads, and the mode follows it). For heavy scenery, where the game thread saturates " +
                   "before the ring; off keeps the mode above."),
                GameOptions.flag(this, GameOptions.JOGO_RAPIDO),
                v -> GameOptions.flag(this, GameOptions.JOGO_RAPIDO, v)));
        rows.add(ui.toggleRow(tr("Cache de vértices entre quadros", "Vertex cache across frames"),
                tr("Reaproveita os vértices que o jogo não mudou desde o quadro anterior, em vez de copiá-los de " +
                   "novo: 87 % menos dados enviados à GPU. No S23 Ultra, parado, +35 % de fps (27 → 36,5); " +
                   "numa paisagem com muita vegetação, de ~30 para ~40 fps.",
                   "Reuses the vertices the game has not changed since the previous frame instead of copying them " +
                   "again: 87 % less data sent to the GPU. On the S23 Ultra, standing still, +35 % fps (27 → 36.5); " +
                   "in a landscape with a lot of vegetation, from ~30 to ~40 fps."),
                GameOptions.flag(this, GameOptions.VERTEX_CACHE),
                v -> GameOptions.flag(this, GameOptions.VERTEX_CACHE, v)));
        rows.add(ui.toggleRow(tr("Cache de índices entre quadros", "Index cache across frames"),
                tr("Reaproveita os índices já convertidos que o jogo não mudou desde o quadro anterior, em vez de " +
                   "convertê-los e copiá-los de novo.",
                   "Reuses the already converted indices the game has not changed since the previous frame, " +
                   "instead of converting and copying them again."),
                GameOptions.flag(this, GameOptions.INDEX_CACHE),
                v -> GameOptions.flag(this, GameOptions.INDEX_CACHE, v)));
        rows.add(ui.toggleRow(tr("Vértices sem espera entre passadas", "Vertices without waiting between passes"),
                tr("A GPU começa a geometria de uma passada enquanto ainda pinta a anterior, em vez de esperar " +
                   "cada passada terminar. A imagem é a mesma.",
                   "The GPU starts a pass's geometry while it still shades the previous one, instead of waiting " +
                   "for every pass to finish. The image is the same."),
                GameOptions.flag(this, GameOptions.VERTEX_NO_WAIT),
                v -> GameOptions.flag(this, GameOptions.VERTEX_NO_WAIT, v)));
        rows.add(ui.toggleRow(tr("Alvos pequenos em tamanho exato", "Small targets at exact size"),
                tr("Cria os alvos de render pequenos (cubo de reflexo) na altura exata em vez de 720 linhas: " +
                   "passes menores e menos tráfego de blocos. Se o jogo resolver abaixo da imagem, ela volta " +
                   "sozinha ao tamanho cheio.",
                   "Creates the small render targets (reflection cube) at their exact height instead of 720 " +
                   "rows: smaller passes and less tile traffic. If the game ever resolves below the image, " +
                   "it grows back to full height on its own."),
                GameOptions.flag(this, GameOptions.EXACT_TARGETS),
                v -> GameOptions.flag(this, GameOptions.EXACT_TARGETS, v)));
        if (GameOptions.PERFORMANCE_HINTS_IN_MENU) {
            rows.add(ui.toggleRow(tr("Dicas de desempenho (ADPF)", "Performance hints (ADPF)"),
                    tr("Diz ao Android quanto cada quadro trabalhou e a meta do limite de FPS, para ele subir " +
                       "o clock do processador quando o quadro não cabe na meta.",
                       "Tells Android how long each frame worked and the FPS limit's target, so it raises the " +
                       "processor's clock when a frame does not fit the target."),
                    GameOptions.flag(this, GameOptions.PERFORMANCE_HINTS),
                    v -> GameOptions.flag(this, GameOptions.PERFORMANCE_HINTS, v)));
        }
        return rows.toArray(new View[0]);
    }

    private LinearLayout driverGroup() {
        String gpu = GpuDrivers.gpuName(this);
        String chosen = chosenDriver();
        LinearLayout group = ui.group();
        group.addView(ui.checkRow(driverTitle(""), probeLine("", gpu.isEmpty() ? null : gpu),
                chosen.isEmpty(), v -> testDriver("", true)));
        for (GpuDrivers.Driver driver : GpuDrivers.list(this)) {
            group.addView(ui.checkRow(driver.name, probeLine(driver.name, driver.library),
                    driver.name.equals(chosen), v -> testDriver(driver.name, true)));
        }
        group.addView(ui.actionRow(tr("Importar driver (.zip)", "Import a driver (.zip)"), null, false, v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("application/zip");
            startActivityForResult(pick, REQUEST_DRIVER_ZIP);
        }));
        LinearLayout delete = ui.actionRow(tr("Apagar o selecionado", "Delete the chosen one"), null, true, v -> {
            GpuDrivers.Driver driver = GpuDrivers.find(this, GameOptions.get(this, GameOptions.DRIVER));
            if (driver == null) return;
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(tr("Apagar " + driver.name + "?", "Delete " + driver.name + "?"))
                    .setMessage(tr("O driver sai do aparelho e o jogo volta ao driver do sistema.",
                            "The driver leaves the phone and the game goes back to the system driver."))
                    .setPositiveButton(tr("Apagar", "Delete"), (d, w) -> {
                        GpuDrivers.remove(this, driver);
                        GameOptions.set(this, GameOptions.DRIVER, "");
                        rebuildPage(PERFORMANCE);
                    })
                    .setNegativeButton(tr("Cancelar", "Cancel"), null)
                    .show();
        });
        Menu.setRowEnabled(delete, !chosen.isEmpty());
        group.addView(delete);
        Menu.refreshDividers(group);
        return group;
    }

    /** Under a driver's name: what its last test found, after the given detail. */
    private String probeLine(String name, String detail) {
        JSONObject probe = GpuDrivers.lastProbe(this, name);
        String state;
        if (name.equals(probingDriver)) state = tr("testando…", "testing…");
        else if (probe == null) state = tr("não testado", "not tested");
        else if (probePassed(probe)) state = "Vulkan " + probe.optString("vulkan", "?") + " · " + tr("ok", "ok");
        else state = tr("falhou no teste", "failed the test");
        return detail == null ? state : detail + " · " + state;
    }

    private static boolean probePassed(JSONObject probe) {
        return probe.optBoolean("compatible", false) && !probe.optBoolean("driverLoadFailed", true);
    }

    // The Vulkan test of the chosen driver (or of the one being tested): its state, then what it found.
    private LinearLayout probeGroup() {
        String name = probingDriver != null ? probingDriver : chosenDriver();
        JSONObject probe = probingDriver != null ? null : GpuDrivers.lastProbe(this, name);
        LinearLayout group = ui.group();
        LinearLayout status;
        if (probingDriver != null) {
            status = ui.statusRow(driverTitle(name), tr("Abrindo o driver e criando um dispositivo Vulkan…",
                    "Opening the driver and creating a Vulkan device…"), Menu.AMBER, tr("Testando", "Testing"));
        } else if (probe == null) {
            status = ui.row(driverTitle(name), tr("Ainda não testado.", "Not tested yet."), null);
        } else {
            boolean passed = probePassed(probe);
            status = ui.statusRow(driverTitle(name), passed
                            ? tr("O jogo consegue usar este driver.", "The game can use this driver.")
                            : tr("O jogo pode não abrir com este driver.", "The game may not start with this driver."),
                    passed, passed ? tr("Compatível", "Compatible") : tr("Falhou", "Failed"));
        }
        group.addView(status);
        if (probe != null) {
            String driverName = probe.optString("driverName", "");
            String driverInfo = probe.optString("driverInfo", "");
            if (probe.has("gpu")) group.addView(detailRow("GPU", probe.optString("gpu")));
            if (!driverName.isEmpty() || !driverInfo.isEmpty()) {
                group.addView(detailRow(tr("Driver", "Driver"),
                        (driverName + (driverInfo.isEmpty() ? "" : " · " + driverInfo)).trim()));
            }
            if (probe.has("vulkan")) group.addView(detailRow("Vulkan", probe.optString("vulkan")));
            if (probe.has("logicalDeviceResult")) {
                int result = probe.optInt("logicalDeviceResult");
                group.addView(detailRow(tr("Dispositivo lógico", "Logical device"), result == 0
                        ? tr("criado", "created") : tr("falhou", "failed") + " (VkResult " + result + ")"));
            }
            JSONArray missing = probe.optJSONArray("missing");
            if (missing != null && missing.length() > 0) {
                StringBuilder list = new StringBuilder();
                for (int i = 0; i < missing.length(); i++) {
                    if (i > 0) list.append('\n');
                    list.append("• ").append(missing.optString(i));
                }
                group.addView(detailRow(tr("Falta", "Missing"), list.toString()));
            }
            if (probe.optBoolean("driverNotLoaded")) group.addView(detailRow(tr("Erro", "Error"), notLoaded()));
            if (probe.has("probeError")) group.addView(detailRow(tr("Erro", "Error"), probe.optString("probeError")));
        }
        LinearLayout again = ui.actionRow(probe == null && probingDriver == null
                ? tr("Testar o driver", "Test the driver") : tr("Testar de novo", "Test again"), null, false,
                v -> testDriver(chosenDriver(), false));
        Menu.setRowEnabled(again, probingDriver == null);
        group.addView(again);
        Menu.refreshDividers(group);
        return group;
    }

    private static String notLoaded() {
        return tr("O driver importado não carregou (falta alguma biblioteca dele, ou não é para esta GPU) e o " +
                  "sistema usou o driver de fábrica no lugar.",
                  "The imported driver did not load (one of its libraries is missing, or it is not for this GPU) " +
                  "and the system used the stock driver instead.");
    }

    private LinearLayout detailRow(String title, String value) {
        LinearLayout row = ui.row(title, value, null);
        Menu.titleOf(row).setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        Menu.titleOf(row).setTextColor(Menu.SECONDARY);
        TextView text = Menu.descriptionOf(row);
        text.setTextColor(Menu.PRIMARY);
        text.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        text.setTextIsSelectable(true);
        return row;
    }

    private String probingDriver;   // the driver under test ("" = system), null when none
    private boolean probeChooses;   // whether the driver under test becomes the chosen one if it passes

    /** Tests a driver in GpuProbeActivity's throwaway process; with choose, picks it if it passes (or if asked to). */
    private void testDriver(String name, boolean choose) {
        if (probingDriver != null) return;
        if (choose && name.equals(chosenDriver())) choose = false;  // already the chosen one: just test it
        probingDriver = name;
        probeChooses = choose;
        rebuildPage(PERFORMANCE);
        try {
            startActivityForResult(new Intent(this, GpuProbeActivity.class)
                    .putExtra(GpuProbeActivity.EXTRA_DRIVER, name), REQUEST_GPU_PROBE);
        } catch (ActivityNotFoundException e) {
            probingDriver = null;
            rebuildPage(PERFORMANCE);
        }
    }

    private void finishDriverTest(int resultCode, Intent data) {
        String name = probingDriver;
        boolean choose = probeChooses;
        probingDriver = null;
        if (name == null) return;
        String report = data == null ? null : data.getStringExtra(GpuProbeActivity.EXTRA_REPORT);
        if (report == null) {
            // The test process died without answering: the driver crashed while loading.
            report = "{\"compatible\":false,\"driverLoadFailed\":true,\"probeError\":" +
                    JSONObject.quote(tr("O driver derrubou o processo de teste ao abrir.",
                            "The driver crashed the test process while opening.")) + "}";
        }
        GpuDrivers.saveProbe(this, name, report);
        android.util.Log.i("FH2", "Vulkan test of " + driverTitle(name) + ": " + report);
        JSONObject probe = GpuDrivers.lastProbe(this, name);
        boolean passed = probe != null && probePassed(probe);
        if (choose && passed) {
            GameOptions.set(this, GameOptions.DRIVER, name);
            toast(tr("Driver escolhido: ", "Driver chosen: ") + driverTitle(name));
        } else if (choose) {
            String reason = probe == null ? "" : probe.optBoolean("driverNotLoaded") ? notLoaded()
                    : probe.optString("probeError", "");
            JSONArray missing = probe == null ? null : probe.optJSONArray("missing");
            if (reason.isEmpty() && missing != null && missing.length() > 0) reason = missing.optString(0);
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(tr("O driver não passou no teste", "The driver failed the test"))
                    .setMessage(driverTitle(name) + (reason.isEmpty() ? "" : "\n\n" + reason) + "\n\n" +
                            tr("Com ele o jogo pode não abrir. Usar mesmo assim?",
                               "The game may not start with it. Use it anyway?"))
                    .setPositiveButton(tr("Usar mesmo assim", "Use it anyway"), (d, w) -> {
                        GameOptions.set(this, GameOptions.DRIVER, name);
                        rebuildPage(PERFORMANCE);
                    })
                    .setNegativeButton(tr("Cancelar", "Cancel"), null)
                    .show();
        }
        rebuildPage(PERFORMANCE);
    }

    // The turbo locks the GPU clock for the whole phone: only with a Turnip chosen, as in the fork.
    private LinearLayout turboGroup() {
        GpuDrivers.Driver selected = GpuDrivers.find(this, GameOptions.get(this, GameOptions.DRIVER));
        boolean turnip = selected != null && selected.isTurnip();
        LinearLayout turbo = ui.toggleRow(tr("Turbo da GPU", "GPU turbo"),
                tr("Fixa o clock da GPU em vez de deixar o sistema ajustá-lo. Só com Turnip em uma GPU Adreno. Nem " +
                   "sempre ajuda: confira com o contador de FPS.",
                   "Locks the GPU clock instead of letting the system adjust it. Only with Turnip on an Adreno GPU. " +
                   "It does not always help: check with the FPS counter."),
                turnip && GameOptions.flag(this, GameOptions.TURBO), v -> GameOptions.flag(this, GameOptions.TURBO, v));
        Menu.setRowEnabled(turbo, turnip);
        return ui.group(turbo);
    }

    private ScrollView soundPage() {
        // Volume group: choice row followed by its footer note.
        LinearLayout volumeBox = ui.group(
                ui.toggleRow(tr("Decodificar áudio em segundo plano", "Decode audio in the background"),
                        tr("Ligado: um thread próprio decodifica o som do jogo e alivia a CPU, mas em alguns " +
                           "celulares o som sai travando ou robótico. Desligado: o jogo decodifica o próprio " +
                           "som, como antes. Vale na próxima vez que o jogo abrir.",
                           "On: a thread of its own decodes the game's sound and spares the CPU, but on some " +
                           "phones the sound stutters or sounds robotic. Off: the game decodes its own sound, " +
                           "as before. Takes effect the next time the game starts."),
                        GameOptions.flag(this, GameOptions.AUDIO_WORKER),
                        v -> GameOptions.flag(this, GameOptions.AUDIO_WORKER, v)),
                ui.choiceRow(GameOptions.VOLUME, tr("Volume do jogo", "Game volume"), null,
                        new String[] {"Normal", tr("Alto", "Loud"), tr("Muito alto", "Very loud"), tr("Máximo", "Max")},
                        new String[] {"100", "125", "150", "200"}));
        TextView volumeFooter = ui.footer(tr("Acima de Normal o som pode distorcer nos picos.",
                "Above Normal the sound may distort at its peaks."));

        // Vibration section: toggle row + mode selector (shown only when vibration is on).
        LinearLayout vibrationModeRow = ui.choiceRow(GameOptions.VIBRATION_MODE, tr("Modo de vibração", "Vibration mode"),
                tr("Escolha o que vibra quando o jogo pede rumble.",
                   "Choose what vibrates when the game requests rumble."),
                new String[] {tr("Somente o celular", "Phone only"),
                              tr("Somente o controle", "Gamepad only"),
                              tr("Celular e controle", "Phone and gamepad")},
                new String[] {"phone", "gamepad", "both"},
                value -> {
                    // Test vibration immediately when the user picks a mode.
                    try {
                        Vibrator vibrator = (Vibrator) getSystemService(VIBRATOR_SERVICE);
                        if (vibrator != null && vibrator.hasVibrator()) {
                            int durationMs = 300;
                            int intensity;
                            if ("phone".equals(value)) intensity = 100;
                            else if ("gamepad".equals(value)) intensity = 60;
                            else intensity = 100; // both
                            vibrator.vibrate(VibrationEffect.createOneShot(durationMs, intensity));
                        }
                    } catch (Throwable t) {
                        android.util.Log.e("FH2Vibration", "vibrate failed: " + t);
                    }
                });
        boolean vibrationOn = GameOptions.flag(this, GameOptions.VIBRATION_PHONE);
        vibrationModeRow.setVisibility(vibrationOn ? View.VISIBLE : View.GONE);

        LinearLayout vibrationBox = ui.group();
        LinearLayout vibrationToggle = ui.toggleRow(tr("Vibração", "Vibration"),
                tr("Faz o celular vibrar quando o controle do jogo vibrar. " +
                   "A intensidade segue o jogo.",
                   "Makes the phone vibrate when the gamepad vibrates. " +
                   "Intensity follows the game."),
                GameOptions.flag(this, GameOptions.VIBRATION_PHONE),
                v -> {
                    GameOptions.flag(this, GameOptions.VIBRATION_PHONE, v);
                    vibrationModeRow.setVisibility(v ? View.VISIBLE : View.GONE);
                });
        vibrationBox.addView(vibrationToggle);
        vibrationBox.addView(vibrationModeRow);

        return pageScroll(ui.pageTitle(pageName(SOUND), null),
                ui.header(tr("Volume", "Volume")),
                volumeBox,
                volumeFooter,
                ui.header(tr("Vibração", "Vibration")),
                vibrationBox);
    }

    // ---- Achievements: the game's, unlocked ones in the app's magenta, locked ones grey with a lock ------------

    private ScrollView achievementsPage() {
        List<Achievements.Entry> all = Achievements.load(this);
        if (all.isEmpty()) {
            return pageScroll(ui.pageTitle(pageName(ACHIEVEMENTS), null),
                    ui.group(ui.row(tr("Nenhuma conquista ainda", "No achievements yet"),
                            tr("A lista vem do próprio jogo: abra o jogo uma vez e ela aparece aqui.",
                               "The list comes from the game itself: open the game once and it shows up here."),
                            null)));
        }
        List<Achievements.Entry> unlocked = new ArrayList<>();
        List<Achievements.Entry> locked = new ArrayList<>();
        int score = 0, total = 0;
        for (Achievements.Entry e : all) {
            total += e.gamerscore;
            if (e.unlocked()) {
                unlocked.add(e);
                score += e.gamerscore;
            } else {
                locked.add(e);
            }
        }
        unlocked.sort((a, b) -> Long.compare(b.unlockedAtMillis, a.unlockedAtMillis));  // the latest first

        List<View> parts = new ArrayList<>();
        parts.add(ui.pageTitle(pageName(ACHIEVEMENTS), null));
        parts.add(achievementsSummary(unlocked.size(), all.size(), score, total));
        if (!unlocked.isEmpty()) {
            parts.add(ui.header(tr("Desbloqueadas", "Unlocked")));
            List<View> rows = new ArrayList<>();
            for (Achievements.Entry e : unlocked) rows.add(achievementRow(e));
            parts.add(ui.group(rows.toArray(new View[0])));
        }
        if (!locked.isEmpty()) {
            parts.add(ui.header(tr("Bloqueadas", "Locked")));
            List<View> rows = new ArrayList<>();
            for (Achievements.Entry e : locked) rows.add(achievementRow(e));
            parts.add(ui.group(rows.toArray(new View[0])));
        }
        return pageScroll(parts.toArray(new View[0]));
    }

    /** How many are unlocked and the gamerscore, with a bar in the app's magenta. */
    private View achievementsSummary(int done, int count, int score, int total) {
        LinearLayout box = ui.column();
        box.setPadding(dp(16), dp(14), dp(16), dp(16));
        LinearLayout line = new LinearLayout(this);
        line.setOrientation(LinearLayout.HORIZONTAL);
        line.setGravity(Gravity.BOTTOM);
        line.addView(ui.text(done + " / " + count, 26, Menu.PRIMARY, true));
        TextView of = ui.text(tr("desbloqueadas", "unlocked"), 14, Menu.SECONDARY, false);
        of.setPadding(dp(8), 0, 0, dp(4));
        line.addView(of, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView points = ui.text(score + " / " + total + " G", 15, Menu.ACCENT_TEXT, true);
        points.setPadding(0, 0, 0, dp(4));
        line.addView(points);
        box.addView(line);

        FrameLayout track = new FrameLayout(this);
        track.setBackground(Menu.rounded(Menu.FILL, dp(3)));
        View fill = new View(this);
        fill.setBackground(Menu.rounded(Menu.ACCENT, dp(3)));
        track.addView(fill, new FrameLayout.LayoutParams(0, FrameLayout.LayoutParams.MATCH_PARENT));
        float fraction = count == 0 ? 0f : done / (float) count;
        track.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            int width = done == 0 ? 0 : Math.max(dp(6), Math.round((r - l) * fraction));
            if (fill.getLayoutParams().width != width) {
                fill.getLayoutParams().width = width;
                fill.post(fill::requestLayout);
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(6));
        lp.topMargin = dp(10);
        box.addView(track, lp);
        return ui.group(box);
    }

    /**
     * One achievement: unlocked ones with their colour icon, a magenta ring and gamerscore, and the day; locked ones
     * grey, their icon without colour under a lock. A secret one says nothing about itself until unlocked.
     */
    private View achievementRow(Achievements.Entry e) {
        boolean got = e.unlocked();
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(14), dp(10), dp(16), dp(10));

        FrameLayout badge = new FrameLayout(this);
        ImageView icon = new ImageView(this);
        icon.setScaleType(ImageView.ScaleType.CENTER_CROP);
        icon.setBackground(Menu.rounded(Menu.FILL, dp(10)));
        icon.setClipToOutline(true);
        if (e.icon != null && !(e.secret && !got)) {
            icon.setImageBitmap(android.graphics.BitmapFactory.decodeFile(e.icon.getAbsolutePath()));
        }
        if (!got) {
            android.graphics.ColorMatrix grey = new android.graphics.ColorMatrix();
            grey.setSaturation(0f);
            icon.setColorFilter(new android.graphics.ColorMatrixColorFilter(grey));
            icon.setImageAlpha(110);
        }
        int side = dp(48);
        FrameLayout.LayoutParams iconParams = new FrameLayout.LayoutParams(side, side, Gravity.CENTER);
        badge.addView(icon, iconParams);
        if (got) {
            badge.setBackground(ring(Menu.ACCENT, dp(13)));
        } else {
            ImageView lock = new ImageView(this);
            lock.setImageResource(R.drawable.ic_cadeado);
            lock.setImageTintList(ColorStateList.valueOf(Menu.PRIMARY));
            lock.setBackground(Menu.rounded(0xCC1B1B1F, dp(9)));
            lock.setPadding(dp(3), dp(3), dp(3), dp(3));
            badge.addView(lock, new FrameLayout.LayoutParams(dp(20), dp(20), Gravity.BOTTOM | Gravity.END));
        }
        row.addView(badge, new LinearLayout.LayoutParams(dp(54), dp(54)));

        LinearLayout labels = ui.column();
        labels.setPadding(dp(14), 0, dp(12), 0);
        boolean hidden = e.secret && !got;
        labels.addView(ui.text(hidden ? tr("Conquista secreta", "Secret achievement") : e.label, 15.5f,
                got ? Menu.PRIMARY : Menu.SECONDARY, got));
        String text = got ? e.description : hidden
                ? tr("Continue jogando para descobrir.", "Keep playing to find out.") : e.lockedDescription;
        if (text != null && !text.isEmpty()) {
            TextView d = ui.text(text, 13, got ? Menu.SECONDARY : Menu.TERTIARY, false);
            d.setLineSpacing(0, 1.1f);
            d.setPadding(0, dp(2), 0, 0);
            labels.addView(d);
        }
        if (got) {
            String day = java.text.DateFormat.getDateInstance(java.text.DateFormat.MEDIUM,
                    AppLanguage.english() ? java.util.Locale.US : new java.util.Locale("pt", "BR"))
                    .format(new java.util.Date(e.unlockedAtMillis));
            TextView when = ui.text(tr("Desbloqueada em ", "Unlocked on ") + day, 12, Menu.ACCENT_TEXT, false);
            when.setPadding(0, dp(3), 0, 0);
            labels.addView(when);
        }
        row.addView(labels, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        TextView points = ui.text(e.gamerscore + " G", 14, got ? Menu.ACCENT_TEXT : Menu.TERTIARY, true);
        points.setPadding(dp(10), dp(4), dp(10), dp(4));
        points.setBackground(Menu.rounded(got ? Menu.ACCENT_SOFT : Menu.FILL, dp(8)));
        row.addView(points);
        return ui.plainRow(row);
    }

    /** A rounded outline, for the unlocked achievements' icons. */
    private android.graphics.drawable.GradientDrawable ring(int color, float radius) {
        android.graphics.drawable.GradientDrawable shape = new android.graphics.drawable.GradientDrawable();
        shape.setCornerRadius(radius);
        shape.setStroke(dp(2), color);
        shape.setColor(Color.TRANSPARENT);
        return shape;
    }

    // ---- Language: the game's (every one on the disc) and the app's ----------------------------------------

    private ScrollView languagePage() {
        String chosen = GameOptions.gameLanguage(this).code;
        LinearLayout game = ui.group();
        for (GameOptions.GameLanguage language : GameOptions.GAME_LANGUAGES) {
            String what = AppLanguage.english() ? language.english : language.portuguese;
            if (language.voiceOver) what += tr(" · dublado", " · voiced");
            game.addView(ui.checkRow(language.name, what, language.code.equals(chosen), v -> {
                GameOptions.set(this, GameOptions.GAME_LANGUAGE, language.code);
                rebuildPage(LANGUAGE);
            }));
        }
        Menu.refreshDividers(game);

        SegmentedControl app = new SegmentedControl(this, new String[] {"Português", "English"});
        app.choose(AppLanguage.english() ? 1 : 0, false);
        app.setOnChoice(index -> {
            GameOptions.set(this, GameOptions.APP_LANGUAGE, index == 1 ? "en" : "pt");
            // After the pill's slide, so the change is seen before the texts swap.
            app.postDelayed(this::rebuildScreen, 240);
        });

        return pageScroll(ui.pageTitle(pageName(LANGUAGE),
                        tr("O idioma do jogo e o deste app.", "The game's language and this app's.")),
                ui.header(tr("App", "App")),
                ui.group(ui.stackedRow(tr("Idioma do app", "App language"),
                        tr("Este menu e o painel dos controles de toque.", "This menu and the touch controls panel."),
                        app)),
                ui.header(tr("Jogo", "Game")),
                game,
                ui.footer(tr("Todos os idiomas do disco: textos, menus e a voz do GPS. Os marcados como dublados " +
                             "também têm as falas no idioma. Vale na próxima vez que o jogo abrir.",
                             "Every language on the disc: texts, menus and the sat nav voice. The ones marked " +
                             "voiced also have the dialogue in that language. Takes effect the next time the game " +
                             "starts.")));
    }

    // ---- Game folder, saves, about -------------------------------------------------------------------------

    private ScrollView gamePage() {
        importStatus = ui.text("", 14, Menu.PRIMARY, false);
        importStatus.setLineSpacing(0, 1.12f);
        checksList = ui.column();
        checksList.setPadding(0, dp(8), 0, 0);
        LinearLayout status = ui.column();
        status.setPadding(dp(16), dp(14), dp(16), dp(12));
        status.addView(importStatus);
        status.addView(checksList);
        selectFolder = ui.actionRow(tr("Escolher pasta do jogo", "Choose the game folder"), null, false,
                view -> selectGameFolder());

        selectIso = ui.actionRow(tr("Usar a ISO do jogo", "Use the game ISO"),
                tr("Lida direto do arquivo: sem extrair nem copiar", "Read straight from the file: no extracting or copying"),
                false, view -> selectIsoFile());
        stopIso = ui.actionRow(tr("Parar de usar a ISO", "Stop using the ISO"),
                tr("Volta para a pasta extraída", "Back to the extracted folder"), true, view -> {
                    GameOptions.set(this, GameOptions.GAME_ISO, "");
                    showGameFolder(null);
                });

        LinearLayout exportSave = ui.actionRow(tr("Exportar save", "Export the save"),
                tr("Para jogos/", "To jogos/") + GAME_FOLDER_NAME + "-saves-exportados", false, v -> {
                    try {
                        File target = SaveTransfer.export(this);
                        toast(tr("Save exportado para jogos/", "Save exported to jogos/") +
                                target.getParentFile().getName() + "/" + target.getName());
                    } catch (IOException e) {
                        toast(tr("Não foi possível exportar: ", "Could not export: ") + e.getMessage());
                    }
                });
        LinearLayout importSave = ui.actionRow(tr("Importar save", "Import a save"),
                tr("De uma exportação ou de um backup automático", "From an export or an automatic backup"),
                false, v -> chooseSaveToImport());
        return pageScroll(ui.pageTitle(pageName(GAME),
                        tr("Os arquivos do jogo e o seu progresso.", "The game files and your progress.")),
                ui.header(tr("Pasta do jogo", "Game folder")),
                ui.group(status, selectIso, stopIso, selectFolder),
ui.footer(tr("Uma ISO do disco (a imagem do Xbox 360) é lida direto, sem ocupar espaço a mais. Ou uma pasta " +
                             "extraída (default.xex, as duas DLLs .xex e media), copiada para Armazenamento interno/" +
                             GAME_FOLDER_NAME + " (7,3 GB). Com a ISO, o fh2_shaders.nfsp do renderizador nativo fica " +
                             "ao lado dela ou em " + GAME_FOLDER_NAME + ".",
                             "A disc image (the Xbox 360 ISO) is read in place, taking no extra space. Or an extracted " +
                             "folder (default.xex, the two .xex DLLs and media), copied to Internal storage/" +
                             GAME_FOLDER_NAME + " (7.3 GB). With the ISO, the native renderer's fh2_shaders.nfsp goes " +
                             "next to it or in " + GAME_FOLDER_NAME + ".")),
                ui.header("Save"),
                ui.group(exportSave, importSave),
                ui.footer(tr("Exportar copia o seu progresso (perfil e saves). Importar troca o progresso atual por " +
                             "uma exportação ou por um dos backups automáticos (jogos/" + GAME_FOLDER_NAME + "-saves). " +
                             "Para levar o save a outro celular, copie a pasta exportada para o mesmo lugar nele e " +
                             "importe.",
                             "Export copies your progress (profile and saves). Import replaces the current progress " +
                             "with an export or one of the automatic backups (jogos/" + GAME_FOLDER_NAME + "-saves). " +
                             "To take the save to another phone, copy the exported folder to the same place on it " +
                             "and import.")));
    }

    private ScrollView aboutPage() {
        LinearLayout hero = new LinearLayout(this);
        hero.setOrientation(LinearLayout.HORIZONTAL);
        hero.setGravity(Gravity.CENTER_VERTICAL);
        hero.setPadding(dp(16), dp(16), dp(16), dp(16));
        hero.addView(appIcon(56), new LinearLayout.LayoutParams(dp(56), dp(56)));
        LinearLayout names = ui.column();
        names.setPadding(dp(14), 0, 0, 0);
        names.addView(ui.text("FH2 Recompiled", 19, Menu.PRIMARY, true));
        names.addView(ui.text(tr("Forza Horizon 2 do Xbox 360, recompilado para Android.",
                "Forza Horizon 2 for the Xbox 360, recompiled for Android."), 13, Menu.SECONDARY, false));
        String version = "";
        try {
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException ignored) {
        }
        TextView versionText = ui.text(tr("Versão ", "Version ") + version, 12, Menu.TERTIARY, false);
        versionText.setPadding(0, dp(4), 0, 0);
        names.addView(versionText);
        hero.addView(names, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        saveReport = ui.actionRow(reportLabel(), null, false, view -> prepareReport(false));
        rootReport = ui.actionRow(rootReportLabel(), tr("Pede permissão de root (Magisk, KernelSU) e junta ao zip " +
                "os crashes completos do jogo, o log do kernel e o logcat inteiro. Só leitura: nada é alterado.",
                "Asks for root (Magisk, KernelSU) and adds the game's full crash files, the kernel log and the whole " +
                "logcat to the zip. Read only: nothing is changed."), false, view -> requestRootReport());
        rootProfile = ui.toggleRow(tr("Gravar perfil de desempenho (root)", "Record a performance profile (root)"),
                tr("Só na próxima partida: depois de 60 s de jogo, grava 10 s com o simpleperf e salva onde o jogo " +
                   "gasta a CPU (por thread e por função) no log da sessão, que entra no diagnóstico. Precisa de " +
                   "root: o pedido de permissão aparece ao ligar, não no meio da partida. Só mede, não muda nada.",
                   "Next game only: after 60 s of play, records 10 s with simpleperf and saves where the game spends " +
                   "the CPU (per thread and per function) in the session log, which goes into the report. Needs " +
                   "root: the permission prompt shows when you turn it on, not during the game. Measures only."),
                GameOptions.flag(this, GameOptions.ROOT_PROFILE), this::setRootProfile);
        return pageScroll(ui.pageTitle(pageName(ABOUT), null),
                ui.group(hero),
                ui.header(tr("Diagnóstico", "Diagnostics")),
                ui.group(saveReport,
                        ui.toggleRow(tr("Medir a GPU nos logs", "Measure the GPU in the logs"),
                        tr("Grava no log quanto tempo a GPU gasta em cada parte do quadro. Só para investigar: " +
                           "deixa o jogo mais lento, porque cada medição faz a GPU esperar.",
                           "Writes to the log how long the GPU spends on each part of the frame. For " +
                           "investigating only: it slows the game down, since each measurement makes the GPU wait."),
                        GameOptions.flag(this, GameOptions.GPU_MEASUREMENTS),
                        v -> GameOptions.flag(this, GameOptions.GPU_MEASUREMENTS, v)),
                        ui.toggleRow(tr("Log detalhado", "Verbose log"),
                        tr("Grava todas as repetições das mensagens que se repetem várias vezes por segundo (texturas " +
                           "do renderizador, funções não implementadas). Desligado, cada uma aparece só as 20 " +
                           "primeiras vezes, e o log guarda mais tempo de jogo. Só para investigar.",
                           "Writes every repetition of the messages that repeat many times a second (renderer " +
                           "textures, unimplemented functions). Off, each shows only its first 20 times, and the log " +
                           "keeps more play time. For investigating only."),
                        GameOptions.flag(this, GameOptions.VERBOSE_LOG),
                        v -> GameOptions.flag(this, GameOptions.VERBOSE_LOG, v)),
                        rootReport,
                        rootProfile),
                ui.footer(tr("Logs e informações do aparelho num .zip, para investigar um problema. Nada é enviado.",
                        "Logs and device information in a .zip, to look into a problem. Nothing is sent.")),
                ui.header(tr("Projeto", "Project")),
                ui.group(ui.actionRow(tr("Gabrick75 no GitHub", "Gabrick75 on GitHub"),
                        PROJECT_URL.replace("https://", ""), false, view -> {
                    try {
                        startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(PROJECT_URL)));
                    } catch (ActivityNotFoundException e) {
                        Toast.makeText(this, PROJECT_URL, Toast.LENGTH_LONG).show();
                    }
                })));
    }

    private static String reportLabel() {
        return tr("Salvar diagnóstico (ZIP)", "Save diagnostics (ZIP)");
    }

    private void importDriver(Uri zip) {
        worker.execute(() -> {
            try {
                GpuDrivers.Driver driver = GpuDrivers.importZip(this, zip);
                mainHandler.post(() -> {
                    // Tested at once, and chosen if it passes.
                    GpuDrivers.saveProbe(this, driver.name, null);
                    toast(tr("Driver importado: ", "Driver imported: ") + driver.name);
                    testDriver(driver.name, true);
                });
            } catch (IOException error) {
                mainHandler.post(() -> toast(tr("Não foi possível importar o driver: ",
                        "Could not import the driver: ") + error.getMessage()));
            }
        });
    }

    /*
     * The emulated renderer needs vertexPipelineStoresAndAtomics; some GPUs lack it (Mali-G1-Ultra on a Xiaomi with
     * Android 17: the game stopped at "Graphics presentation setup failed: C0000001", while the native renderer ran).
     * From the Vulkan test of the chosen driver when it was tested, else of the system's. Slow the first time (it
     * opens Vulkan): off the UI thread only. false when unknown.
     */
    static boolean emulatedRendererUnsupported(android.content.Context context) {
        JSONObject probe = null;
        String driver = GameOptions.get(context, GameOptions.DRIVER);
        if (!driver.isEmpty()) probe = GpuDrivers.lastProbe(context, driver);
        if (probe == null) probe = Diagnostics.gpu();
        return probe != null && probe.has("vertexPipelineStoresAndAtomics")
                && !probe.optBoolean("vertexPipelineStoresAndAtomics", true);
    }

    private static String emulatedUnsupportedMessage() {
        return tr("⚠ A GPU deste aparelho não tem um recurso que o renderizador emulado exige " +
                  "(vertexPipelineStoresAndAtomics): com ele o jogo não abre. Use o renderizador nativo.",
                  "⚠ This phone's GPU lacks a feature the emulated renderer needs " +
                  "(vertexPipelineStoresAndAtomics): with it the game does not start. Use the native renderer.");
    }

    // ---- Play ----------------------------------------------------------------------------------------------

    private void play() {
        launchGame.setEnabled(false);
        Diagnostics.recordLaunch(this);
        worker.execute(() -> {
            // The edition check reads default.xex from the folder; an ISO is mounted as it is (not checked).
            GameEdition edition = GameIso.chosen(this) != null ? null : GameEdition.of(this, gameRoot());
            boolean emulatedBlocked = !GameOptions.nativeRenderer(this) && emulatedRendererUnsupported(this);
            mainHandler.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                launchGame.setEnabled(true);
                if (emulatedBlocked) {
                    new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle(tr("Renderizador emulado incompatível", "Emulated renderer not supported"))
                            .setMessage(emulatedUnsupportedMessage())
                            .setPositiveButton(tr("Usar o nativo", "Use native"), (dialog, which) -> {
                                GameOptions.set(this, GameOptions.RENDERER, "native");
                                rebuildPage(GRAPHICS);
                                startActivity(new Intent(this, GameActivity.class));
                            })
                            .setNeutralButton(tr("Jogar mesmo assim", "Play anyway"),
                                    (dialog, which) -> startActivity(new Intent(this, GameActivity.class)))
                            .setNegativeButton(tr("Voltar", "Back"), null)
                            .show();
                    return;
                }
                if (edition != null && !edition.supported()) {
                    new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle(tr("Edição do jogo incompatível", "Unsupported game edition"))
                            .setMessage(tr("Este app foi recompilado a partir da edição " + GameEdition.SUPPORTED_NAME
                                    + " e o seu default.xex é de outra (SHA-256 " + edition.sha256.substring(0, 12)
                                    + "…). Com outra edição o jogo não roda.\n\nUse uma cópia da edição "
                                    + GameEdition.SUPPORTED_NAME + " (o SHA-256 do default.xex começa com "
                                    + GameEdition.SUPPORTED_SHA256.substring(0, 12) + ").",
                                    "This app was recompiled from the " + GameEdition.SUPPORTED_NAME
                                    + " edition and your default.xex is from another one (SHA-256 "
                                    + edition.sha256.substring(0, 12) + "…). The game does not run with another "
                                    + "edition.\n\nUse a copy of the " + GameEdition.SUPPORTED_NAME
                                    + " edition (the SHA-256 of its default.xex starts with "
                                    + GameEdition.SUPPORTED_SHA256.substring(0, 12) + ")."))
                            .setPositiveButton(tr("Voltar", "Back"), null)
                            .setNeutralButton(tr("Jogar mesmo assim", "Play anyway"),
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
        addCheck(tr("Pasta media", "media folder"), new File(folder, "media").isDirectory());
    }

    private void addCheck(String name, boolean ok) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(3), 0, dp(3));
        row.addView(ui.dot(ok ? Menu.GREEN : Menu.RED));
        TextView label = ui.text(name, 13.5f, ok ? Menu.SECONDARY : Menu.RED, false);
        label.setPadding(dp(10), 0, 0, 0);
        row.addView(label);
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
        Menu.setRowEnabled(selectFolder, false);
        setGameReady(false);
        setImportStatus(storageNotice());
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
        Menu.setRowEnabled(selectFolder, true);
        launchGame.setEnabled(true);
        // The native renderer's requirement row follows the shader library copied in (or removed) meanwhile.
        if (shadersFound != null && shadersFound != shaderLibrary(this).isFile()) rebuildPage(GRAPHICS);
        File iso = GameIso.chosen(this);
        selectIso.setVisibility(View.VISIBLE);
        stopIso.setVisibility(iso != null ? View.VISIBLE : View.GONE);
        Menu.titleOf(selectIso).setText(iso != null ? tr("Trocar a ISO", "Change the ISO")
                : tr("Usar a ISO do jogo", "Use the game ISO"));
        Menu.refreshDividers((android.view.ViewGroup) selectIso.getParent());
        if (iso != null) {
            // The game reads the disc image in place: no folder to check.
            checksList.removeAllViews();
            addCheck(iso.getName(), true);
            setImportStatus((notice == null ? "" : notice + "\n") + tr("Lendo direto da ISO: ", "Reading straight from the ISO: ") +
                    iso.getAbsolutePath());
            setGameReady(true);
            Menu.titleOf(selectFolder).setText(tr("Usar uma pasta extraída", "Use an extracted folder"));
            return;
        }
        showChecks(root);
        if (isValidGameFolder(root)) {
            setImportStatus((notice == null ? "" : notice + "\n") + tr("Armazenamento interno/", "Internal storage/") +
                    GAME_FOLDER_NAME);
            setGameReady(true);
            Menu.titleOf(selectFolder).setText(tr("Trocar pasta do jogo", "Change the game folder"));
        } else {
            setImportStatus(tr("Escolha a pasta extraída do jogo (com default.xex, as duas DLLs .xex e media). " +
                            "Ela será copiada para Armazenamento interno/" + GAME_FOLDER_NAME + " (7,3 GB).",
                    "Choose the game's extracted folder (with default.xex, the two .xex DLLs and media). It will " +
                            "be copied to Internal storage/" + GAME_FOLDER_NAME + " (7.3 GB)."));
            setGameReady(false);
            Menu.titleOf(selectFolder).setText(tr("Escolher pasta do jogo", "Choose the game folder"));
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

    private void selectIsoFile() {
        if (!hasStorageAccess()) {
            prepareGameFolder();
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");  // .iso has no MIME type every file manager knows
        startActivityForResult(intent, REQUEST_ISO);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_GPU_PROBE) {
            finishDriverTest(resultCode, data);
            return;
        }
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
            else setImportStatus(storageNotice());
            return;
        }
        if (requestCode == REQUEST_ISO) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) useIso(data.getData());
            return;
        }
        if (requestCode != REQUEST_GAME_FOLDER || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri tree = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException error) {
            setImportStatus(tr("Não foi possível guardar a permissão da pasta: ", "Could not keep the folder's permission: ") +
                    error.getMessage());
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
        Menu.setRowEnabled(selectFolder, false);
        setGameReady(false);
        launchGame.setEnabled(false);
        playStatus.setText(tr("Copiando o jogo…", "Copying the game…"));
        setImportStatus(tr("Copiando o jogo para Armazenamento interno/", "Copying the game to Internal storage/") +
                GAME_FOLDER_NAME + "…");
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
                if (source == null || !source.isDirectory()) throw new IOException(tr("Não foi possível abrir a pasta.", "Could not open the folder."));
                copyDirectory(source, staging, staging.getCanonicalFile(), new long[] {0});
                if (!isValidGameFolder(staging)) {
                    throw new IOException(tr("A pasta deve conter default.xex, XMediaFacade_default.xex, " +
                            "SpeechFacade_default.xex e media.", "The folder must have default.xex, " +
                            "XMediaFacade_default.xex, SpeechFacade_default.xex and media."));
                }
                if (root.exists()) Files.move(root.toPath(), previous.toPath(), StandardCopyOption.REPLACE_EXISTING);
                Files.move(staging.toPath(), root.toPath(), StandardCopyOption.REPLACE_EXISTING);
                deleteRecursively(previous);
                mainHandler.post(() -> showGameFolder(tr("Jogo copiado.", "Game copied.")));
            } catch (Exception error) {
                try { deleteRecursively(staging); } catch (IOException ignored) { }
                mainHandler.post(() -> {
                    setImportStatus(tr("Erro ao importar: ", "Import failed: ") + error.getMessage());
                    Menu.setRowEnabled(selectFolder, true);
                    launchGame.setEnabled(true);
                    setGameReady(false);
                });
            } finally {
                mainHandler.post(() -> getWindow().clearFlags(
                        android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
            }
        });
    }

    // Keeps the path of the chosen ISO after checking it is an Xbox 360 disc image; the game mounts it in place.
    // The path is found on the worker: it may open the file through the picker's provider.
    private void useIso(Uri uri) {
        worker.execute(() -> {
            File iso = GameIso.pathOf(this, uri);
            boolean found = iso != null && iso.isFile();
            String problem = found ? GameIso.check(iso) : null;
            mainHandler.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                if (!found) {
                    message(tr("Não foi possível achar o caminho dessa ISO. Escolha o arquivo pelo armazenamento " +
                               "interno ou pelo cartão de memória (no seletor, menu ☰ → nome do aparelho), não por " +
                               "\"Recentes\".",
                               "Could not find that ISO's path. Pick the file through the internal storage or the " +
                               "memory card (in the picker, menu ☰ → the device name), not through \"Recent\"."));
                    return;
                }
                if (problem != null) {
                    message(iso.getName() + ": " + problem);
                    return;
                }
                GameOptions.set(this, GameOptions.GAME_ISO, iso.getAbsolutePath());
                showGameFolder(tr("ISO escolhida.", "ISO chosen."));
                if (shadersFound != null && shadersFound != shaderLibrary(this).isFile()) rebuildPage(GRAPHICS);
            });
        });
    }

    private void copyDirectory(DocumentFile source, File destination, File safeRoot, long[] copiedBytes)
            throws IOException {
        if (!destination.getCanonicalFile().toPath().startsWith(safeRoot.toPath())) {
            throw new IOException(tr("A pasta do jogo contém um caminho inválido.", "The game folder has an invalid path."));
        }
        if (!destination.mkdirs() && !destination.isDirectory()) throw new IOException(tr("Não foi possível criar ", "Could not create ") + destination);
        for (DocumentFile child : source.listFiles()) {
            String name = child.getName();
            if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") ||
                    name.contains("/") || name.contains("\\")) {
                throw new IOException(tr("A pasta do jogo contém um nome inválido.", "The game folder has an invalid name."));
            }
            File target = new File(destination, name);
            if (child.isDirectory()) {
                copyDirectory(child, target, safeRoot, copiedBytes);
            } else if (child.isFile()) {
                try (InputStream input = getContentResolver().openInputStream(child.getUri());
                     FileOutputStream output = new FileOutputStream(target)) {
                    if (input == null) throw new IOException(tr("Não foi possível ler ", "Could not read ") + name);
                    byte[] buffer = new byte[1024 * 1024];
                    for (int count; (count = input.read(buffer)) != -1; ) {
                        output.write(buffer, 0, count);
                        copiedBytes[0] += count;
                        if ((copiedBytes[0] & ((64L * 1024 * 1024) - 1)) < count) {
                            long total = copiedBytes[0];
                            mainHandler.post(() -> setImportStatus(tr("Copiados ", "Copied ") +
                                    (total / (1024 * 1024)) + tr(" MiB de ~7500…", " MiB of ~7500…")));
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
        if (!file.delete()) throw new IOException(tr("Não foi possível apagar ", "Could not delete ") + file.getName());
    }

    // ---- Diagnostic report (saved by the player; never sent anywhere) --------------------------------------

    private static String rootReportLabel() {
        return tr("Diagnóstico com root (ZIP)", "Diagnostics with root (ZIP)");
    }

    // The root manager's prompt first (off the UI thread, it waits for the player), then the usual report flow.
    private void requestRootReport() {
        Menu.setRowEnabled(rootReport, false);
        Menu.titleOf(rootReport).setText(tr("Pedindo root…", "Asking for root…"));
        worker.execute(() -> {
            boolean granted = RootDiagnostics.available();
            mainHandler.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                Menu.setRowEnabled(rootReport, true);
                Menu.titleOf(rootReport).setText(rootReportLabel());
                if (granted) {
                    prepareReport(true);
                } else {
                    message(tr("Sem root: o aparelho não tem root ou a permissão foi negada. O diagnóstico comum " +
                            "continua disponível.", "No root: the phone is not rooted or the permission was denied. " +
                            "The usual diagnostics are still available."));
                }
            });
        });
    }

    // The profile is recorded through su during the next game: the root manager's prompt is asked for here, when the
    // option is turned on, not in the middle of a race. Denied, the option goes back off.
    private void setRootProfile(boolean on) {
        GameOptions.flag(this, GameOptions.ROOT_PROFILE, on);
        if (!on) return;
        Menu.setRowEnabled(rootProfile, false);
        worker.execute(() -> {
            boolean granted = RootDiagnostics.available();
            mainHandler.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                Menu.setRowEnabled(rootProfile, true);
                if (granted) return;
                Menu.switchOf(rootProfile).setChecked(false);  // saves it off too
                message(tr("Sem root: o aparelho não tem root ou a permissão foi negada. O perfil de desempenho " +
                        "não será gravado.", "No root: the phone is not rooted or the permission was denied. The " +
                        "performance profile will not be recorded."));
            });
        });
    }

    // First what happened, in the player's words: it goes into relatorio.txt, where the report used to end with
    // an unanswered "describe what happened".
    private void prepareReport(boolean root) {
        android.widget.EditText description = new android.widget.EditText(this);
        description.setHint(tr("O que aconteceu? Como reproduzir? (opcional)",
                "What happened? How to reproduce it? (optional)"));
        description.setMinLines(3);
        description.setMaxLines(8);
        description.setGravity(Gravity.TOP | Gravity.START);
        description.setInputType(android.text.InputType.TYPE_CLASS_TEXT
                | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE | android.text.InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        android.widget.FrameLayout box = new android.widget.FrameLayout(this);
        box.setPadding(dp(20), dp(8), dp(20), 0);
        box.addView(description);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(tr("Diagnóstico", "Diagnostics"))
                .setView(box)
                .setPositiveButton(tr("Criar relatório", "Create report"),
                        (dialog, which) -> createReport(description.getText().toString(), root))
                .setNegativeButton(tr("Cancelar", "Cancel"), null)
                .show();
    }

    private void createReport(String description, boolean root) {
        LinearLayout row = root ? rootReport : saveReport;
        String label = root ? rootReportLabel() : reportLabel();
        Menu.setRowEnabled(row, false);
        Menu.titleOf(row).setText(tr("Preparando diagnóstico…", "Preparing diagnostics…"));
        worker.execute(() -> {
            try {
                File report = Diagnostics.createReport(this, description, root);
                mainHandler.post(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    Menu.setRowEnabled(row, true);
                    Menu.titleOf(row).setText(label);
                    pendingReport = report;
                    Intent save = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                    save.addCategory(Intent.CATEGORY_OPENABLE);
                    save.setType("application/zip");
                    save.putExtra(Intent.EXTRA_TITLE, report.getName());
                    try {
                        startActivityForResult(save, REQUEST_SAVE_REPORT);
                    } catch (ActivityNotFoundException error) {
                        pendingReport = null;
                        message(tr("Não há um app para salvar arquivos.", "There is no app to save files."));
                    }
                });
            } catch (Exception error) {
                mainHandler.post(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    Menu.setRowEnabled(row, true);
                    Menu.titleOf(row).setText(label);
                    message(tr("Não foi possível preparar o relatório: ", "Could not prepare the report: ") +
                            error.getMessage());
                });
            }
        });
    }

    private void finishReportSave(Uri destination) {
        File report = pendingReport;
        pendingReport = null;
        if (report == null || !report.isFile()) {
            message(tr("O relatório não está mais disponível. Gere outro.", "The report is no longer there. Make another."));
            return;
        }
        worker.execute(() -> {
            try (InputStream input = new FileInputStream(report);
                 OutputStream output = getContentResolver().openOutputStream(destination, "w")) {
                if (output == null) throw new IOException(tr("Não foi possível abrir o arquivo de destino.",
                        "Could not open the target file."));
                byte[] buffer = new byte[16384];
                for (int count; (count = input.read(buffer)) != -1; ) output.write(buffer, 0, count);
                mainHandler.post(() -> message(tr("Diagnóstico salvo.", "Diagnostics saved.")));
            } catch (Exception error) {
                mainHandler.post(() -> message(tr("Não foi possível salvar o ZIP: ", "Could not save the ZIP: ") + error.getMessage()));
            }
        });
    }

    private void message(String text) {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(tr("Diagnóstico", "Diagnostics")).setMessage(text).setPositiveButton("OK", null).show();
    }

    // ---- Small helpers -------------------------------------------------------------------------------------

    private static String storageNotice() {
        return tr("Para usar Armazenamento interno/" + GAME_FOLDER_NAME + ", ative o acesso a arquivos que o " +
                "Android pede.", "To use Internal storage/" + GAME_FOLDER_NAME + ", allow the file access Android " +
                "asks for.");
    }

    private void setImportStatus(String text) {
        if (importStatus != null) importStatus.setText(text);
    }

    private void toast(String text) {
        Toast.makeText(this, text, Toast.LENGTH_LONG).show();
    }

    private static ColorStateList byEnabled(int disabled, int enabled) {
        return new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}}, new int[] {disabled, enabled});
    }

    private int dp(float value) {
        return Menu.dp(this, value);
    }

    @Override
    protected void onDestroy() {
        worker.shutdown();
        super.onDestroy();
    }
}
