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

import com.google.android.material.button.MaterialButton;
import com.google.android.material.slider.Slider;

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
    private static final String TREE_URI = "tree_uri";
    // Relative to the internal storage root (the "jogos" folder holds the user's games).
    static final String GAME_FOLDER_NAME = "jogos/ForzaHorizon2";
    static final String PROJECT_URL = "https://github.com/Gabrick75/fh2-recompiled";

    private TextView importStatus;
    private LinearLayout checksList;
    private LinearLayout selectFolder;
    private MaterialButton launchGame;
    private LinearLayout saveReport;
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
            toast("Nenhum save para importar em jogos/" + GAME_FOLDER_NAME + "-saves-exportados ou -saves.");
            return;
        }
        String[] names = new String[saves.size()];
        for (int i = 0; i < names.length; i++) {
            File f = saves.get(i);
            names[i] = f.getName() + (f.getParentFile().equals(SaveTransfer.exportsFolder()) ? " · exportado"
                                                                                               : " · backup");
        }
        new android.app.AlertDialog.Builder(this)
                .setTitle("Importar save")
                .setItems(names, (dialog, which) -> new android.app.AlertDialog.Builder(this)
                        .setTitle("Trocar o progresso atual?")
                        .setMessage("O save de " + names[which] + " substitui o atual. O atual continua no backup " +
                                "automático da última vez que o jogo abriu.")
                        .setPositiveButton("Importar", (d, w) -> importSave(saves.get(which)))
                        .setNegativeButton("Cancelar", null)
                        .show())
                .show();
    }

    private void importSave(File source) {
        try {
            SaveTransfer.importFrom(this, source);
            toast("Save importado de " + source.getName());
        } catch (IOException e) {
            toast("Não foi possível importar: " + e.getMessage());
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
        if (hasStorageAccess()) {
            showGameFolder(null);
        }
    }

    // ---- Screen --------------------------------------------------------------------------------------------
    // A sidebar with the pages, one under the other, and the Play button always at hand at its foot; on the right
    // the chosen page, in grouped lists (Menu). Graphics comes first and starts with the renderer: the options under
    // it are the chosen renderer's own, so the emulated and the native ones never mix.

    private static final int GRAPHICS = 0, DISPLAY = 1, PERFORMANCE = 2, SOUND = 3, GAME = 4, ABOUT = 5;
    private static final String[] PAGE_NAMES = {"Gráficos", "Tela", "Desempenho", "Som", "Jogo e saves", "Sobre"};
    private static final int[] PAGE_ICONS = {R.drawable.ic_menu_graficos, R.drawable.ic_menu_tela,
            R.drawable.ic_menu_desempenho, R.drawable.ic_menu_som, R.drawable.ic_menu_jogo, R.drawable.ic_menu_sobre};
    private static final String STATE_PAGE = "page";

    private Menu ui;
    private FrameLayout content;
    private final ScrollView[] pages = new ScrollView[PAGE_NAMES.length];
    private final LinearLayout[] navRows = new LinearLayout[PAGE_NAMES.length];
    private int currentPage = -1;
    private TextView playStatus;
    private View playDot;
    private boolean gameReady;
    private LinearLayout rendererOptions;
    private TextView rendererNote;
    private Boolean shadersFound;  // what the native renderer's requirement row shows; null before it is built

    private View buildScreen(int page) {
        ui = new Menu(this);
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

    private View sidebar() {
        LinearLayout bar = ui.column();
        bar.setBackgroundColor(Menu.SIDEBAR);
        bar.setPadding(dp(12), dp(14), dp(12), dp(12));

        LinearLayout brand = new LinearLayout(this);
        brand.setOrientation(LinearLayout.HORIZONTAL);
        brand.setGravity(Gravity.CENTER_VERTICAL);
        brand.setPadding(dp(6), 0, dp(6), dp(12));
        brand.addView(appIcon(34), new LinearLayout.LayoutParams(dp(34), dp(34)));
        LinearLayout names = ui.column();
        names.setPadding(dp(10), 0, 0, 0);
        names.addView(ui.text("FH2 Recompiled", 16, Menu.PRIMARY, true));
        names.addView(ui.text("Xbox 360 · Android", 12, Menu.SECONDARY, false));
        brand.addView(names);
        bar.addView(brand);

        LinearLayout nav = ui.column();
        for (int i = 0; i < PAGE_NAMES.length; i++) {
            navRows[i] = navRow(i);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(40));
            lp.bottomMargin = dp(2);
            nav.addView(navRows[i], lp);
        }
        ScrollView navScroll = new ScrollView(this);
        navScroll.setVerticalScrollBarEnabled(false);
        navScroll.setVerticalFadingEdgeEnabled(true);  // a short screen (or a big font) fades the list out, not cuts it
        navScroll.setFadingEdgeLength(dp(24));
        navScroll.addView(nav);
        bar.addView(navScroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout status = new LinearLayout(this);
        status.setOrientation(LinearLayout.HORIZONTAL);
        status.setGravity(Gravity.CENTER_VERTICAL);
        status.setPadding(dp(6), dp(8), dp(6), dp(8));
        playDot = ui.dot(Menu.TERTIARY);
        status.addView(playDot);
        playStatus = ui.text("", 12.5f, Menu.SECONDARY, false);
        playStatus.setPadding(dp(8), 0, 0, 0);
        status.addView(playStatus);
        bar.addView(status);

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
        bar.addView(launchGame, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(48)));
        setGameReady(false);
        return bar;
    }

    private ImageView appIcon(int sizeDp) {
        ImageView icon = new ImageView(this);
        icon.setImageResource(R.mipmap.ic_launcher);
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
        TextView label = ui.text(PAGE_NAMES[page], 15, Menu.PRIMARY, false);
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

    /** The Play button and the line above it: Play when the game is in place, otherwise a way to the Game page. */
    private void setGameReady(boolean ready) {
        gameReady = ready;
        launchGame.setText(ready ? "Jogar" : "Instalar o jogo");
        launchGame.setBackgroundTintList(byEnabled(Menu.FILL, ready ? Menu.ACCENT : Menu.FILL));
        launchGame.setTextColor(byEnabled(Menu.TERTIARY, ready ? Color.WHITE : Menu.ACCENT_TEXT));
        launchGame.setElevation(ready ? dp(6) : 0);
        playStatus.setText(ready ? "Pronto para jogar" : "Jogo não encontrado");
        ((android.graphics.drawable.GradientDrawable) playDot.getBackground()).setColor(ready ? Menu.GREEN : Menu.AMBER);
    }

    private ScrollView buildPage(int page) {
        switch (page) {
            case GRAPHICS: return graphicsPage();
            case DISPLAY: return displayPage();
            case PERFORMANCE: return performancePage();
            case SOUND: return soundPage();
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
        SegmentedControl renderer = new SegmentedControl(this, new String[] {"Emulado", "Nativo"});
        renderer.choose(nativeRenderer ? 1 : 0, false);
        rendererNote = ui.footer("");
        rendererOptions = ui.column();
        fillRendererOptions(nativeRenderer);
        renderer.setOnChoice(index -> {
            GameOptions.set(this, GameOptions.RENDERER, index == 1 ? "native" : "emulated");
            fillRendererOptions(index == 1);
            Menu.enter(rendererOptions);
        });
        return pageScroll(ui.pageTitle("Gráficos", "Escolha o renderizador: as opções abaixo são as dele."),
                ui.header("Renderizador"),
                ui.group(ui.stackedRow("Como o jogo é desenhado", null, renderer)),
                rendererNote,
                rendererOptions);
    }

    private void fillRendererOptions(boolean nativeRenderer) {
        rendererNote.setText(nativeRenderer
                ? "Nativo (experimental): desenha os comandos do jogo direto em Vulkan, com os shaders traduzidos. " +
                  "Bem mais rápido, mas ainda tem erros de imagem."
                : "Emulado: a GPU do Xbox 360 emulada. É o modo testado e o mais fiel à imagem do console.");
        rendererOptions.removeAllViews();
        shadersFound = null;
        if (nativeRenderer) {
            boolean library = shaderLibrary().isFile();
            shadersFound = library;
            rendererOptions.addView(ui.header("Requisito"));
            rendererOptions.addView(ui.group(ui.statusRow("Biblioteca de shaders",
                    "fh2_shaders.nfsp, junto dos arquivos do jogo (" + GAME_FOLDER_NAME + "). Sem ela o nativo " +
                            "não tem os shaders traduzidos.",
                    library, library ? "Encontrada" : "Faltando")));

            rendererOptions.addView(ui.header("Imagem do nativo"));
            SegmentedControl aa = new SegmentedControl(this, new String[] {"Desligada", "FXAA"});
            aa.choose("fxaa".equals(GameOptions.get(this, GameOptions.ANTI_ALIASING)) ? 1 : 0, false);
            aa.setOnChoice(index -> GameOptions.set(this, GameOptions.ANTI_ALIASING, index == 1 ? "fxaa" : "off"));
            rendererOptions.addView(ui.group(
                    renderScaleRow(true, null),
                    ui.stackedRow("Suavização de bordas",
                            "O FXAA tira o serrilhado com um filtro barato sobre a imagem final.", aa),
                    anisotropicRow()));
            rendererOptions.addView(ui.footer("O nativo não tem MSAA: as superfícies 4x do jogo são desenhadas " +
                    "com uma amostra."));

            rendererOptions.addView(ui.header("Desempenho do nativo"));
            rendererOptions.addView(ui.group(onePassRow(
                    "Sem MSAA a cena cabe inteira e é desenhada uma vez só, em vez de em 3 faixas: menos trabalho " +
                            "para a CPU (+25 %).")));
        } else {
            rendererOptions.addView(ui.header("Imagem do emulado"));
            LinearLayout image = ui.group();
            Runnable[] refreshScale = new Runnable[1];
            LinearLayout msaa = ui.choiceRow(GameOptions.MSAA_SAMPLES, "Amostras do MSAA",
                    "4x é o que o jogo usa. 2x suaviza menos e custa menos (+37 %).",
                    new String[] {"2x", "4x"}, new String[] {"2", "4"}, v -> refreshScale[0].run());
            LinearLayout fxaa = ui.choiceRow(GameOptions.FXAA_QUALITY, "Qualidade do FXAA",
                    "Alta suaviza mais bordas, inclusive as fracas, e deixa a imagem um pouco mais suave.",
                    new String[] {"Normal", "Alta"}, new String[] {"normal", "high"});
            Runnable showModes = () -> {
                String mode = GameOptions.get(this, GameOptions.ANTI_ALIASING);
                msaa.setVisibility("msaa".equals(mode) ? View.VISIBLE : View.GONE);
                fxaa.setVisibility("fxaa".equals(mode) ? View.VISIBLE : View.GONE);
                Menu.refreshDividers(image);
            };
            image.addView(renderScaleRow(false, refreshScale));
            image.addView(ui.choiceRow(GameOptions.ANTI_ALIASING, "Suavização de bordas",
                    "Tira o serrilhado. O MSAA é o do jogo e o mais caro para a GPU (desligado rende +91 %); o " +
                            "FXAA é um filtro barato sobre a imagem final.",
                    new String[] {"Desligada", "MSAA", "FXAA"}, new String[] {"off", "msaa", "fxaa"}, v -> {
                        showModes.run();
                        if ("msaa".equals(v)) Menu.enter(msaa);
                        if ("fxaa".equals(v)) Menu.enter(fxaa);
                        refreshScale[0].run();
                    }));
            image.addView(msaa);
            image.addView(fxaa);
            image.addView(anisotropicRow());
            showModes.run();
            rendererOptions.addView(image);

            rendererOptions.addView(ui.header("Desempenho do emulado"));
            rendererOptions.addView(ui.group(
                    onePassRow("O jogo desenha a cena em 3 faixas por causa do MSAA; sem ele a cena cabe inteira e " +
                            "é desenhada uma vez só: menos trabalho para a CPU (+25 %). Só vale com a suavização " +
                            "desligada ou FXAA."),
                    ui.toggleRow("Cópias diretas para texturas",
                            "Experimental. Quando o jogo copia uma imagem que desenhou para usá-la como textura, " +
                                    "copia direto na GPU, sem passar pela memória do console emulado. Desligue se " +
                                    "aparecer alguma imagem errada.",
                            GameOptions.flag(this, GameOptions.RESOLVE_SHORTCUT),
                            v -> GameOptions.flag(this, GameOptions.RESOLVE_SHORTCUT, v))));
        }
        rendererOptions.addView(ui.header("Ainda sem efeito"));
        rendererOptions.addView(ui.group(
                ui.choiceRow(GameOptions.FPS, "Limite de FPS",
                        "O teto do jogo. 90, 120 e Livre são experimentais e pedem uma tela com essa taxa.",
                        new String[] {"30", "60", "90", "120", "Livre"}, new String[] {"30", "60", "90", "120", "0"}),
                ui.choiceRow(GameOptions.SCALING, "Escala para a tela",
                        "Como a imagem é levada ao tamanho da tela. O FSR é mais nítido quando o jogo desenha " +
                                "abaixo da resolução da tela; o CAS só aumenta a nitidez.",
                        new String[] {"Bilinear", "FSR 1.0", "CAS"}, new String[] {"bilinear", "fsr", "cas"}),
                ui.choiceRow(GameOptions.SHADOWS, "Sombras",
                        "A cada quanto os mapas de sombras são redesenhados. A cada 2 quadros custa menos e quase " +
                                "não se nota.",
                        new String[] {"Todo quadro", "A cada 2"}, new String[] {"1", "2"}),
                ui.choiceRow(GameOptions.SHADOW_DISTANCE, "Distância das sombras",
                        "Até onde os objetos projetam sombra: é aí que está quase todo o custo delas. Longe é " +
                                "como o jogo.",
                        new String[] {"Longe", "Média", "Perto"}, new String[] {"far", "medium", "near"}),
                ui.choiceRow(GameOptions.CAR_REFLECTIONS, "Reflexos do carro",
                        "Faces do mapa de reflexos do carro redesenhadas a cada quadro.",
                        new String[] {"Altos", "Médios", "Baixos"}, new String[] {"high", "medium", "low"})));
        rendererOptions.addView(ui.footer("Ficam guardadas, mas o jogo ainda não as aplica. Por enquanto o limite " +
                "de FPS só escolhe a taxa de atualização da tela."));
    }

    // The native renderer's translated shaders (nfsmw_nativo_sistema.cpp also looks next to the executable).
    private static File shaderLibrary() {
        return new File(gameRoot(), "fh2_shaders.nfsp");
    }

    private LinearLayout anisotropicRow() {
        return ui.choiceRow(GameOptions.ANISOTROPIC, "Filtro anisotrópico",
                "Texturas mais nítidas vistas em ângulo, como a estrada ao longe.",
                new String[] {"Não", "2x", "4x", "16x"}, new String[] {"0", "2", "3", "5"});
    }

    private LinearLayout onePassRow(String description) {
        return ui.toggleRow("Cena em uma passada", description, GameOptions.flag(this, GameOptions.ONE_PASS),
                v -> GameOptions.flag(this, GameOptions.ONE_PASS, v));
    }

    /**
     * Render resolution, as a percentage of the Xbox 360's 1280x720, with the size it really becomes. Emulated: in
     * eighths (12.5 % steps, quarters with MSAA 4x); native: 50 to 100, any value. refreshOut gets the refresh, for
     * the anti-aliasing choice that changes the emulated rounding.
     */
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
            slider = ui.slider(50, 100, 1, GameOptions.nativeRenderScalePercent(this));
        } else {
            slider = ui.slider(1, 8, 1, GameOptions.renderScaleEighths(GameOptions.renderScalePercent(this), false));
        }
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(2);
        box.addView(slider, lp);

        Runnable show = () -> {
            if (nativeRenderer) {
                // Same rounding as Fh2Escalar (nfsmw_nativo_destinos.cpp): the width in multiples of 16.
                int value = GameOptions.nativeRenderScalePercent(this);
                int width = value >= 100 ? 1280 : ((1280 * value / 100) + 15) & ~15;
                percent.setText(value + " %");
                size.setText(width + "×" + (width * 720 / 1280) + (value >= 100 ? " · a resolução original" : ""));
                return;
            }
            int eighths = GameOptions.renderScaleEighths(GameOptions.renderScalePercent(this), GameOptions.msaa4x(this));
            percent.setText((eighths % 2 == 0 ? String.valueOf(eighths * 25 / 2) : (eighths * 25 / 2) + ",5") + " %");
            size.setText((1280 * eighths / 8) + "×" + (720 * eighths / 8) +
                    (eighths == 8 ? " · a resolução original" : ""));
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
        return ui.stackedRow("Resolução de renderização", nativeRenderer
                ? "Quanto da resolução do Xbox 360 (1280×720) o jogo desenha: a cena e o pós-processamento (o HUD " +
                  "também fica menos nítido). Abaixo de 100 % a GPU trabalha bem menos."
                : "Quanto da resolução do Xbox 360 (1280×720) o jogo desenha, em passos de 12,5 %. Abaixo de 100 % " +
                  "a imagem fica menos nítida e a GPU trabalha bem menos.", box);
    }

    // ---- Display, performance, sound -----------------------------------------------------------------------

    private ScrollView displayPage() {
        return pageScroll(ui.pageTitle("Tela", "A imagem final e o que aparece por cima dela."),
                ui.header("Imagem"),
                ui.group(resolutionRow(),
                        ui.toggleRow("Esticar para a tela inteira",
                                "Desligado: a imagem 16:9 do Xbox 360, com barras pretas. Ligado: preenche a tela, " +
                                        "mas distorce a imagem.",
                                GameOptions.stretch(this), v -> GameOptions.stretch(this, v))),
                ui.header("Apresentação"),
                ui.group(ui.choiceRow(GameOptions.PRESENT_MODE, "Modo de apresentação",
                                "FIFO espera o vsync da tela: é o mais rápido (+57 % no menu) e esquenta menos. " +
                                        "MAILBOX repinta sem parar: um pouco menos de atraso, mas tira CPU e GPU do jogo.",
                                new String[] {"FIFO · vsync", "MAILBOX"}, new String[] {"fifo", "mailbox"}),
                        ui.toggleRow("Vblank adaptativo",
                                "Um quadro que perde o vsync por pouco é mostrado na hora, em vez de esperar o " +
                                        "próximo. Não muda o fps; desligue se notar trepidação.",
                                GameOptions.flag(this, GameOptions.ADAPTIVE_VBLANK),
                                v -> GameOptions.flag(this, GameOptions.ADAPTIVE_VBLANK, v))),
                ui.header("Sobreposição"),
                ui.group(ui.toggleRow("Mostrar FPS", "Contador no canto: quadros por segundo e milissegundos por quadro.",
                                GameOptions.flag(this, GameOptions.SHOW_FPS),
                                v -> GameOptions.flag(this, GameOptions.SHOW_FPS, v)),
                        ui.toggleRow("Mostrar uso de GPU, CPU e RAM",
                                "Abaixo do FPS: ocupação da GPU; CPU do jogo (de todos os núcleos) e a thread mais " +
                                        "ocupada (perto de 100 % é ela que limita o FPS); memória do jogo e a livre do " +
                                        "aparelho, em GB.",
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
        labels.add("Nativa");
        values.add(GameOptions.RES_NATIVE);
        return ui.choiceRow(GameOptions.RESOLUTION, "Resolução",
                "Tamanho da imagem final na tela (a nativa é " + longSide + "×" + shortSide + "). O jogo é " +
                        "renderizado do mesmo jeito.",
                labels.toArray(new String[0]), values.toArray(new String[0]));
    }

    private ScrollView performancePage() {
        return pageScroll(ui.pageTitle("Desempenho", "O processador e o driver da GPU."),
                ui.header("Processador"),
                ui.group(ui.toggleRow("Fixar threads nos núcleos",
                        "Deixa o jogo só nos núcleos rápidos do processador e o processador de comandos da GPU (o " +
                                "do emulado ou o do nativo) sozinho no mais rápido (+16 %).",
                        GameOptions.flag(this, GameOptions.PIN_THREADS),
                        v -> GameOptions.flag(this, GameOptions.PIN_THREADS, v))),
                ui.header("Driver da GPU"),
                driverGroup(),
                ui.footer(GpuDrivers.isAdreno(this)
                        ? "Turnip é o driver livre do Mesa para Adreno: importe o .zip de um driver Turnip (com " +
                          "meta.json) e escolha-o aqui. Por enquanto o jogo sempre usa o driver do sistema."
                        : "Turnip, o driver livre do Mesa, é só para GPUs Adreno: com esta GPU o jogo usa o driver " +
                          "do sistema."),
                ui.header("Ainda sem efeito"),
                turboGroup());
    }

    // System driver or an imported one (Turnip), import / delete.
    private LinearLayout driverGroup() {
        String gpu = GpuDrivers.gpuName(this);
        String chosen = GameOptions.get(this, GameOptions.DRIVER);
        if (GpuDrivers.find(this, chosen) == null) chosen = "";
        LinearLayout group = ui.group();
        group.addView(ui.checkRow("Driver do sistema", gpu.isEmpty() ? null : gpu, chosen.isEmpty(),
                v -> chooseDriver("")));
        for (GpuDrivers.Driver driver : GpuDrivers.list(this)) {
            group.addView(ui.checkRow(driver.name, driver.isTurnip() ? "Turnip" : null, driver.name.equals(chosen),
                    v -> chooseDriver(driver.name)));
        }
        group.addView(ui.actionRow("Importar driver (.zip)", null, false, v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("application/zip");
            startActivityForResult(pick, REQUEST_DRIVER_ZIP);
        }));
        GpuDrivers.Driver selected = GpuDrivers.find(this, chosen);
        LinearLayout delete = ui.actionRow("Apagar o selecionado", null, true, v -> {
            GpuDrivers.Driver driver = GpuDrivers.find(this, GameOptions.get(this, GameOptions.DRIVER));
            if (driver == null) return;
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle("Apagar " + driver.name + "?")
                    .setMessage("O driver sai do aparelho e o jogo volta ao driver do sistema.")
                    .setPositiveButton("Apagar", (d, w) -> {
                        GpuDrivers.remove(driver);
                        GameOptions.set(this, GameOptions.DRIVER, "");
                        rebuildPage(PERFORMANCE);
                    })
                    .setNegativeButton("Cancelar", null)
                    .show();
        });
        Menu.setRowEnabled(delete, selected != null);
        group.addView(delete);
        Menu.refreshDividers(group);
        return group;
    }

    private void chooseDriver(String name) {
        GameOptions.set(this, GameOptions.DRIVER, name);
        rebuildPage(PERFORMANCE);
    }

    // The turbo locks the GPU clock for the whole phone: only with a Turnip chosen, as in the fork.
    private LinearLayout turboGroup() {
        GpuDrivers.Driver selected = GpuDrivers.find(this, GameOptions.get(this, GameOptions.DRIVER));
        boolean turnip = selected != null && selected.isTurnip();
        LinearLayout turbo = ui.toggleRow("Turbo da GPU",
                "Fixa o clock da GPU em vez de deixar o sistema ajustá-lo. Só com Turnip em uma GPU Adreno. Nem " +
                        "sempre ajuda: confira com o contador de FPS.",
                turnip && GameOptions.flag(this, GameOptions.TURBO), v -> GameOptions.flag(this, GameOptions.TURBO, v));
        Menu.setRowEnabled(turbo, turnip);
        return ui.group(turbo);
    }

    private ScrollView soundPage() {
        return pageScroll(ui.pageTitle("Som", null),
                ui.header("Volume"),
                ui.group(ui.choiceRow(GameOptions.VOLUME, "Volume do jogo", null,
                        new String[] {"Normal", "Alto", "Muito alto", "Máximo"},
                        new String[] {"100", "125", "150", "200"})),
                ui.footer("Acima de Normal o som pode distorcer nos picos."));
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
        selectFolder = ui.actionRow("Escolher pasta do jogo", null, false, view -> selectGameFolder());

        LinearLayout exportSave = ui.actionRow("Exportar save", "Para jogos/" + GAME_FOLDER_NAME + "-saves-exportados",
                false, v -> {
                    try {
                        File target = SaveTransfer.export(this);
                        toast("Save exportado para jogos/" + target.getParentFile().getName() + "/" + target.getName());
                    } catch (IOException e) {
                        toast("Não foi possível exportar: " + e.getMessage());
                    }
                });
        LinearLayout importSave = ui.actionRow("Importar save", "De uma exportação ou de um backup automático",
                false, v -> chooseSaveToImport());
        return pageScroll(ui.pageTitle("Jogo e saves", "Os arquivos do jogo e o seu progresso."),
                ui.header("Pasta do jogo"),
                ui.group(status, selectFolder),
                ui.footer("A pasta extraída do disco (default.xex, as duas DLLs .xex e media) é copiada para " +
                        "Armazenamento interno/" + GAME_FOLDER_NAME + " (7,3 GB)."),
                ui.header("Save"),
                ui.group(exportSave, importSave),
                ui.footer("Exportar copia o seu progresso (perfil e saves). Importar troca o progresso atual por uma " +
                        "exportação ou por um dos backups automáticos (jogos/" + GAME_FOLDER_NAME + "-saves). Para " +
                        "levar o save a outro celular, copie a pasta exportada para o mesmo lugar nele e importe."));
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
        names.addView(ui.text("Forza Horizon 2 do Xbox 360, recompilado para Android.", 13, Menu.SECONDARY, false));
        String version = "";
        try {
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException ignored) {
        }
        TextView versionText = ui.text("Versão " + version, 12, Menu.TERTIARY, false);
        versionText.setPadding(0, dp(4), 0, 0);
        names.addView(versionText);
        hero.addView(names, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        saveReport = ui.actionRow("Salvar diagnóstico (ZIP)", null, false, view -> prepareReport());
        return pageScroll(ui.pageTitle("Sobre", null),
                ui.group(hero),
                ui.header("Diagnóstico"),
                ui.group(saveReport),
                ui.footer("Logs e informações do aparelho num .zip, para investigar um problema. Nada é enviado."),
                ui.header("Projeto"),
                ui.group(ui.actionRow("Gabrick75 no GitHub", PROJECT_URL.replace("https://", ""), false, view -> {
                    try {
                        startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(PROJECT_URL)));
                    } catch (ActivityNotFoundException e) {
                        Toast.makeText(this, PROJECT_URL, Toast.LENGTH_LONG).show();
                    }
                })));
    }

    private void importDriver(Uri zip) {
        worker.execute(() -> {
            try {
                GpuDrivers.Driver driver = GpuDrivers.importZip(this, zip);
                mainHandler.post(() -> {
                    GameOptions.set(this, GameOptions.DRIVER, driver.name);
                    rebuildPage(PERFORMANCE);
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
        Menu.setRowEnabled(selectFolder, true);
        launchGame.setEnabled(true);
        showChecks(root);
        // The native renderer's requirement row follows the shader library copied in (or removed) meanwhile.
        if (shadersFound != null && shadersFound != shaderLibrary().isFile()) rebuildPage(GRAPHICS);
        if (isValidGameFolder(root)) {
            setImportStatus((notice == null ? "" : notice + "\n") + "Armazenamento interno/" + GAME_FOLDER_NAME);
            setGameReady(true);
            Menu.titleOf(selectFolder).setText("Trocar pasta do jogo");
        } else {
            setImportStatus("Escolha a pasta extraída do jogo (com default.xex, as duas DLLs .xex e media). " +
                    "Ela será copiada para Armazenamento interno/" + GAME_FOLDER_NAME + " (7,3 GB).");
            setGameReady(false);
            Menu.titleOf(selectFolder).setText("Escolher pasta do jogo");
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
        Menu.setRowEnabled(selectFolder, false);
        setGameReady(false);
        launchGame.setEnabled(false);
        playStatus.setText("Copiando o jogo…");
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
        Menu.setRowEnabled(saveReport, false);
        Menu.titleOf(saveReport).setText("Preparando diagnóstico…");
        worker.execute(() -> {
            try {
                File report = Diagnostics.createReport(this);
                mainHandler.post(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    Menu.setRowEnabled(saveReport, true);
                    Menu.titleOf(saveReport).setText("Salvar diagnóstico (ZIP)");
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
                    Menu.setRowEnabled(saveReport, true);
                    Menu.titleOf(saveReport).setText("Salvar diagnóstico (ZIP)");
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

    // ---- Small helpers -------------------------------------------------------------------------------------

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
