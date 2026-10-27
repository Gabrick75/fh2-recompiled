package com.forzahorizon2.android;

import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Context;
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
import android.view.TextureView;
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
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(AppLanguage.wrap(base));
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        setContentView(buildScreen(state == null ? HOME : state.getInt(STATE_PAGE, HOME)));
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
            toast(tr(R.string.no_save_to_import_in_jogos) + GAME_FOLDER_NAME +
                    tr(R.string.saves_exportados_or_saves));
            return;
        }
        String[] names = new String[saves.size()];
        for (int i = 0; i < names.length; i++) {
            File f = saves.get(i);
            names[i] = f.getName() + (f.getParentFile().equals(SaveTransfer.exportsFolder()) ? tr(R.string.export)
                                                                                               : " · backup");
        }
        new android.app.AlertDialog.Builder(this)
                .setTitle(tr(R.string.import_a_save))
                .setItems(names, (dialog, which) -> new android.app.AlertDialog.Builder(this)
                        .setTitle(tr(R.string.replace_the_current_progress))
                        .setMessage(tr(R.string.the_save_from_replaces_the_current, names[which]))
                        .setPositiveButton(tr(R.string.import_save), (d, w) -> importSave(saves.get(which)))
                        .setNegativeButton(tr(R.string.cancel), null)
                        .show())
                .show();
    }

    private void importSave(File source) {
        try {
            SaveTransfer.importFrom(this, source);
            toast(tr(R.string.save_imported_from) + source.getName());
        } catch (IOException e) {
            toast(tr(R.string.could_not_import) + e.getMessage());
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
        resumed = true;
        if (currentPage == HOME) homeVideo.resume();
        // Back from the game: it may have unlocked some.
        if (content != null && pages[ACHIEVEMENTS] != null) rebuildPage(ACHIEVEMENTS);
        if (hasStorageAccess()) {
            showGameFolder(null);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        resumed = false;
        homeVideo.pause();
    }

    // ---- Screen --------------------------------------------------------------------------------------------
    // A sidebar with the pages, one under the other, and the Play button always at hand at its foot; on the right
    // the chosen page, in grouped lists (Menu). The app opens on the home page, which the name at the top of the
    // sidebar brings back: the game's own "press start" video behind the player's name and a big Play (the sidebar's
    // Play hides there, the big one takes its place). Graphics comes first and starts with the renderer: the options under
    // it are the chosen renderer's own, so the emulated and the native ones never mix. Every text is in the app's
    // three languages (AppLanguage.tr); changing the language builds the screen again.

    private static final int ACHIEVEMENTS = 0, GAME = 1, LANGUAGE = 2, DISPLAY = 3, GRAPHICS = 4, PERFORMANCE = 5,
            SOUND = 6, ABOUT = 7;
    private static final int HOME = 8;  // not in the sidebar's list: its name at the top opens it
    private static final int[] PAGE_ICONS = {R.drawable.ic_menu_conquistas, R.drawable.ic_menu_jogo,
            R.drawable.ic_menu_idioma, R.drawable.ic_menu_tela, R.drawable.ic_menu_graficos,
            R.drawable.ic_menu_desempenho, R.drawable.ic_menu_outros, R.drawable.ic_menu_sobre};
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
    private LinearLayout brand;
    private View homeView;
    private MaterialButton homePlay;
    private TextView homeName;
    private ImageView homeSound;
    private HomeVideo homeVideo;
    private String homeVideoSource;  // what the video was opened from: opened again only when that changes
    private boolean resumed;

    private static String pageName(int page) {
        switch (page) {
            case GRAPHICS: return tr(R.string.graphics);
            case DISPLAY: return tr(R.string.display);
            case PERFORMANCE: return tr(R.string.performance);
            case SOUND: return tr(R.string.others);
            case ACHIEVEMENTS: return tr(R.string.achievements);
            case LANGUAGE: return tr(R.string.language);
            case GAME: return tr(R.string.game_and_saves);
            default: return tr(R.string.about);
        }
    }

    private View buildScreen(int page) {
        ui = new Menu(this);
        currentPage = -1;
        // The page under the sidebar: the home page's video runs behind its see-through panel, the other pages
        // start past its slanted edge (showPage).
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Menu.BACKGROUND);
        content = new FrameLayout(this);
        root.addView(content, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        root.addView(sidebar(), new FrameLayout.LayoutParams(dp(SIDEBAR_WIDTH), FrameLayout.LayoutParams.MATCH_PARENT));
        for (int i = 0; i < pages.length; i++) pages[i] = buildPage(i);
        homeView = homePage();
        showPage(page == HOME || (page >= 0 && page < pages.length) ? page : HOME, false);
        return root;
    }

    private static final int SIDEBAR_WIDTH = 292, SIDEBAR_SLANT = 88;  // dp: the top's width, and how much the foot loses

    private View sidebar() {
        NeonShapes.SlantedPanel panel =
                new NeonShapes.SlantedPanel(dp(SIDEBAR_SLANT), getResources().getDisplayMetrics().density);
        // The rows run the panel's whole width and are cut along its slanted edge.
        LinearLayout bar = new LinearLayout(this) {
            @Override
            protected void dispatchDraw(android.graphics.Canvas canvas) {
                canvas.save();
                canvas.clipPath(panel.shape());
                super.dispatchDraw(canvas);
                canvas.restore();
            }
        };
        bar.setOrientation(LinearLayout.VERTICAL);
        bar.setBackground(panel);
        bar.setPadding(0, dp(14), 0, dp(12));

        // The name, and under it whether the game is ready to play: all of it one button, to the home page.
        brand = new LinearLayout(this);
        brand.setOrientation(LinearLayout.HORIZONTAL);
        brand.setGravity(Gravity.CENTER_VERTICAL);
        brand.setPadding(dp(24), dp(12), dp(12), dp(12));
        brand.setContentDescription(tr(R.string.home));
        brand.setOnClickListener(v -> showPage(HOME, true));
        Menu.pressable(brand);
        ImageView logo = appIcon(42);
        logo.setForeground(stroked(Color.TRANSPARENT, 0x99E6007E, dp(42 * 0.24f)));  // the speedometer, ringed in magenta
        brand.addView(logo, new LinearLayout.LayoutParams(dp(42), dp(42)));
        LinearLayout names = ui.column();
        names.setPadding(dp(12), 0, 0, 0);
        SpannableStringBuilder title = new SpannableStringBuilder("FH2 RECOMPILED");
        title.setSpan(new android.text.style.ForegroundColorSpan(Menu.ACCENT_TEXT), 4, title.length(),
                Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        TextView titleView = ui.text("", 16, Menu.PRIMARY, true);
        titleView.setText(title);
        titleView.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC));
        titleView.setLetterSpacing(0.02f);
        titleView.setSingleLine(true);
        names.addView(titleView);
        LinearLayout status = new LinearLayout(this);
        status.setOrientation(LinearLayout.HORIZONTAL);
        status.setGravity(Gravity.CENTER_VERTICAL);
        playDot = ui.dot(Menu.TERTIARY);
        LinearLayout.LayoutParams dotParams = new LinearLayout.LayoutParams(dp(7), dp(7));
        status.addView(playDot, dotParams);
        playStatus = ui.text("", 12, 0xFFB4B4BC, false);
        playStatus.setPadding(dp(6), 0, 0, 0);
        playStatus.setSingleLine(true);
        status.addView(playStatus);
        names.addView(status);
        brand.addView(names);
        LinearLayout.LayoutParams brandParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        brandParams.bottomMargin = dp(10);
        bar.addView(brand, brandParams);

        LinearLayout nav = ui.column();
        for (int i = 0; i < navRows.length; i++) {
            navRows[i] = navRow(i);
            nav.addView(navRows[i], new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(46)));
        }
        ScrollView navScroll = new ScrollView(this);
        navScroll.setVerticalScrollBarEnabled(false);
        navScroll.setVerticalFadingEdgeEnabled(true);  // a short screen (or a big font) fades the list out, not cuts it
        navScroll.setFadingEdgeLength(dp(24));
        navScroll.addView(nav);
        bar.addView(navScroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        launchGame = new MaterialButton(this);
        stylePlay(launchGame, 17, 20);
        launchGame.setOnClickListener(view -> {
            if (gameReady) play();
            else showPage(GAME, true);
        });
        Menu.pressable(launchGame);
        LinearLayout.LayoutParams playParams =
                new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(SIDE_PLAY_HEIGHT));
        // The glow's room hangs into the margins; the plate stays clear of the slanted edge at the foot.
        playParams.setMargins(dp(14 - SIDE_PLAY_GLOW), dp(8 - SIDE_PLAY_GLOW), dp(SIDEBAR_SLANT + 6 - SIDE_PLAY_GLOW),
                -dp(SIDE_PLAY_GLOW));
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
        row.setPadding(dp(24), 0, dp(12), 0);
        ImageView icon = new ImageView(this);
        icon.setImageResource(PAGE_ICONS[page]);
        row.addView(icon, new LinearLayout.LayoutParams(dp(22), dp(22)));
        TextView label = ui.text(pageName(page), 15, Menu.PRIMARY, false);
        label.setPadding(dp(16), 0, 0, 0);
        label.setSingleLine(true);
        row.addView(label);
        row.setOnClickListener(v -> showPage(page, true));
        Menu.pressable(row);
        return row;
    }

    private void showPage(int page, boolean animate) {
        if (page == currentPage) return;
        currentPage = page;
        View shown = page == HOME ? homeView : pages[page];
        content.removeAllViews();
        content.addView(shown);
        if (animate) Menu.enter(shown);
        boolean home = page == HOME;
        brand.setBackground(home ? chosenRow() : null);  // chosen as the pages' rows are
        content.setPadding(home ? 0 : dp(SIDEBAR_WIDTH), 0, 0, 0);
        launchGame.setVisibility(home ? View.GONE : View.VISIBLE);
        if (home && resumed) homeVideo.resume();
        else homeVideo.pause();
        for (int i = 0; i < navRows.length; i++) {
            boolean chosen = i == page;
            LinearLayout row = navRows[i];
            row.setBackground(chosen ? chosenRow() : null);
            ((ImageView) row.getChildAt(0)).setImageTintList(
                    ColorStateList.valueOf(chosen ? Menu.ACCENT_TEXT : 0xFFA1A1A9));
            TextView label = (TextView) row.getChildAt(1);
            label.setTextColor(chosen ? Menu.PRIMARY : 0xFFD1D1D6);
            label.setTypeface(Typeface.create("sans-serif-medium", chosen ? Typeface.BOLD : Typeface.NORMAL));
        }
    }

    /** The chosen sidebar row: a neon magenta strip at its left edge over a faint magenta wash fading right. */
    private android.graphics.drawable.Drawable chosenRow() {
        android.graphics.drawable.GradientDrawable wash = new android.graphics.drawable.GradientDrawable(
                android.graphics.drawable.GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] {0x59E6007E, 0x24E6007E, 0x00E6007E});  // out to the slanted edge, fading all the way
        android.graphics.drawable.GradientDrawable strip = new android.graphics.drawable.GradientDrawable();
        strip.setColor(0xFFFF2E9A);
        android.graphics.drawable.LayerDrawable layers =
                new android.graphics.drawable.LayerDrawable(new android.graphics.drawable.Drawable[] {wash, strip});
        layers.setLayerWidth(1, dp(3));
        layers.setLayerGravity(1, Gravity.START | Gravity.FILL_VERTICAL);
        return layers;
    }

    /** A rounded plate with a hairline border. */
    private static android.graphics.drawable.GradientDrawable stroked(int color, int border, float radius) {
        android.graphics.drawable.GradientDrawable shape = Menu.rounded(color, radius);
        shape.setStroke(1, border);
        return shape;
    }

    /** A Play button's text: bold italic capitals, the play mark at their left (PlayBackground draws the rest). */
    private void stylePlay(MaterialButton button, float sp, int iconDp) {
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        button.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC));
        button.setAllCaps(true);
        button.setLetterSpacing(0.04f);
        button.setIconSize(dp(iconDp));
        button.setIconPadding(dp(10));
        button.setIconGravity(MaterialButton.ICON_GRAVITY_TEXT_START);
        button.setIconTint(ColorStateList.valueOf(Color.WHITE));
        button.setSingleLine(true);
        button.setAllCaps(true);  // again: setSingleLine took the capitals away
        button.setInsetTop(0);
        button.setInsetBottom(0);
        button.setLayerType(View.LAYER_TYPE_SOFTWARE, null);  // PlayBackground's blurred glow
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            button.setOutlineSpotShadowColor(Menu.ACCENT);  // a magenta glow instead of a grey shadow
            button.setOutlineAmbientShadowColor(Menu.ACCENT);
        }
    }

    /** A Play button's look with the game present or missing (its click is the same as before). */
    private void showPlay(MaterialButton button, boolean ready, int glowDp, int heightDp) {
        button.setBackgroundTintList(null);
        PlayBackground background = new PlayBackground(getResources().getDisplayMetrics().density, ready, glowDp);
        button.setBackground(background);
        // Centred on the plate, not on the bands or the glow's room around it.
        int height = dp(heightDp);
        button.setPaddingRelative(Math.round(background.inset() + background.lean(height) / 2), 0,
                Math.round(background.inset() + background.stripesWidth(height) + background.rightLean(height) / 2), 0);
        button.setIcon(ready ? getDrawable(R.drawable.ic_jogar) : null);
        button.setTextColor(byEnabled(0xCCD1D1D6, ready ? Color.WHITE : Menu.ACCENT_TEXT));
        button.setElevation(0);  // PlayBackground glows by itself
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putInt(STATE_PAGE, currentPage);
    }

    /** The Play button and the line under the name: Play when the game is in place, otherwise a way to it. */
    private void setGameReady(boolean ready) {
        gameReady = ready;
        launchGame.setText(ready ? tr(R.string.play) : tr(R.string.install_the_game));
        showPlay(launchGame, ready, SIDE_PLAY_GLOW, SIDE_PLAY_HEIGHT);
        playStatus.setText(ready ? tr(R.string.ready_to_play) : tr(R.string.game_not_found));
        ((android.graphics.drawable.GradientDrawable) playDot.getBackground()).setColor(ready ? Menu.GREEN : Menu.AMBER);
        if (homePlay != null) showHomeReady();
    }

    // ---- Home: the game's video behind the player's name and a big Play -----------------------------------------

    private static final int HOME_GREY = 0xFF3A3A3F;  // the home page with no video: no game, or not read yet
    private static final int HOME_CARD = 0x9E17171B;  // see-through over the video
    // dp: the Play buttons' heights, a plate and the glow's room around it (72 + 2 × 14 and 48 + 2 × 8)
    private static final int HOME_PLAY_HEIGHT = 100, HOME_PLAY_GLOW = 14, SIDE_PLAY_HEIGHT = 64, SIDE_PLAY_GLOW = 8;

    private View homePage() {
        FrameLayout home = new FrameLayout(this);
        home.setBackgroundColor(HOME_GREY);

        // The video and a light veil over it (a little more at the foot, under the cards), faded in together.
        FrameLayout videoLayer = new FrameLayout(this);
        TextureView video = new TextureView(this);
        videoLayer.addView(video, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        View veil = new View(this);
        veil.setBackground(new android.graphics.drawable.GradientDrawable(
                android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM, new int[] {0x33000000, 0x8C000000}));
        videoLayer.addView(veil, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        home.addView(videoLayer, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        if (homeVideo != null) homeVideo.close();
        homeVideo = new HomeVideo(video, videoLayer);
        homeVideoSource = null;

        // The sound, top right: off unless the player turned it on (kept for the next time).
        homeSound = new ImageView(this);
        homeSound.setScaleType(ImageView.ScaleType.CENTER);
        homeSound.setImageTintList(ColorStateList.valueOf(Color.WHITE));
        homeSound.setBackground(stroked(0x99000000, 0x33FFFFFF, dp(22)));
        homeSound.setOnClickListener(v -> {
            boolean sound = !GameOptions.flag(this, GameOptions.HOME_VIDEO_SOUND);
            GameOptions.flag(this, GameOptions.HOME_VIDEO_SOUND, sound);
            homeVideo.setSound(sound);
            showHomeSound();
        });
        Menu.pressable(homeSound);
        FrameLayout.LayoutParams soundParams = new FrameLayout.LayoutParams(dp(44), dp(44), Gravity.TOP | Gravity.END);
        soundParams.setMargins(0, dp(20), dp(24), 0);
        home.addView(homeSound, soundParams);
        showHomeSound();

        // Bottom right: the player's name in its card, and under it Play in its own.
        LinearLayout cards = ui.column();
        LinearLayout nameCard = new LinearLayout(this);
        nameCard.setOrientation(LinearLayout.HORIZONTAL);
        nameCard.setGravity(Gravity.CENTER_VERTICAL);
        nameCard.setPadding(dp(22), dp(10), dp(26), dp(10));  // clear of its slanted sides
        nameCard.setBackground(new NeonShapes.FadedBorderCard(HOME_CARD, dp(14), getResources().getDisplayMetrics().density));
        ImageView avatar = new ImageView(this);
        avatar.setImageResource(R.drawable.ic_usuario);
        avatar.setImageTintList(ColorStateList.valueOf(Menu.ACCENT_TEXT));
        avatar.setScaleType(ImageView.ScaleType.CENTER);
        avatar.setBackground(stroked(Menu.ACCENT_SOFT, 0x80E6007E, dp(22)));
        nameCard.addView(avatar, new LinearLayout.LayoutParams(dp(44), dp(44)));
        LinearLayout nameText = ui.column();
        nameText.setPadding(dp(14), 0, dp(8), 0);
        TextView nameLabel = ui.text(tr(R.string.player_name).toUpperCase(java.util.Locale.getDefault()), 11,
                0xFFA1A1A9, false);
        nameLabel.setLetterSpacing(0.08f);
        nameText.addView(nameLabel);
        homeName = ui.text(GameOptions.gamertag(this), 17, Menu.PRIMARY, true);
        homeName.setSingleLine(true);
        homeName.setEllipsize(android.text.TextUtils.TruncateAt.END);
        nameText.addView(homeName);
        nameCard.addView(nameText, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        ImageView edit = new ImageView(this);
        edit.setImageResource(R.drawable.ic_editar);
        edit.setImageTintList(ColorStateList.valueOf(0xFFD1D1D6));
        nameCard.addView(edit, new LinearLayout.LayoutParams(dp(22), dp(22)));
        nameCard.setContentDescription(tr(R.string.edit_player_name));
        nameCard.setOnClickListener(v -> editPlayerName());
        Menu.pressable(nameCard);
        LinearLayout.LayoutParams nameParams = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        nameParams.leftMargin = dp(32);  // a little narrower than Play, starting where its plate's top does
        cards.addView(nameCard, nameParams);

        homePlay = new MaterialButton(this);
        stylePlay(homePlay, 26, 30);
        homePlay.setOnClickListener(view -> {
            if (gameReady) play();
            else showPage(GAME, true);
        });
        Menu.pressable(homePlay);
        LinearLayout.LayoutParams playParams =
                new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(HOME_PLAY_HEIGHT));
        // The glow's margin hangs outside the column: the plate lines up with the name card.
        int glowRoom = dp(HOME_PLAY_GLOW);
        playParams.setMargins(-glowRoom, dp(12) - glowRoom, -glowRoom, -glowRoom);
        cards.setClipChildren(false);
        cards.setClipToPadding(false);
        home.setClipChildren(false);
        cards.addView(homePlay, playParams);

        FrameLayout.LayoutParams cardsParams = new FrameLayout.LayoutParams(dp(340),
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.END);
        cardsParams.setMargins(dp(24), dp(24), dp(24), dp(24));
        home.addView(cards, cardsParams);
        // On a narrow screen the cards take what is left beside the margins.
        home.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            int width = Math.min(dp(340), r - l - dp(48));
            if (width > 0 && cards.getLayoutParams().width != width) {
                cards.getLayoutParams().width = width;
                cards.post(cards::requestLayout);
            }
        });
        showHomeReady();
        return home;
    }

    /** The big Play follows the sidebar's, and the video follows the game files (none: the grey). */
    private void showHomeReady() {
        homePlay.setText(gameReady ? tr(R.string.play) : tr(R.string.install_the_game));
        showPlay(homePlay, gameReady, HOME_PLAY_GLOW, HOME_PLAY_HEIGHT);
        String source = gameReady ? GameIso.gameDataRoot(this).getAbsolutePath() : "";
        if (source.equals(homeVideoSource)) return;
        homeVideoSource = source;
        boolean playable = gameReady &&
                homeVideo.setSource(this, GameOptions.flag(this, GameOptions.HOME_VIDEO_SOUND));
        if (!gameReady) homeVideo.close();
        homeSound.setVisibility(playable ? View.VISIBLE : View.GONE);
    }

    private void showHomeSound() {
        boolean sound = GameOptions.flag(this, GameOptions.HOME_VIDEO_SOUND);
        homeSound.setImageResource(sound ? R.drawable.ic_som_ligado : R.drawable.ic_som_desligado);
        homeSound.setContentDescription(tr(sound ? R.string.video_sound_off : R.string.video_sound_on));
    }

    /** A button of the name dialog: Play's plate without its bands, magenta (save) or dark (cancel). */
    private MaterialButton dialogButton(String text, boolean primary) {
        MaterialButton button = new MaterialButton(this);
        button.setText(text);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        button.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC));
        button.setAllCaps(true);
        button.setLetterSpacing(0.04f);
        button.setInsetTop(0);
        button.setInsetBottom(0);
        button.setMinWidth(dp(120));
        button.setLayerType(View.LAYER_TYPE_SOFTWARE, null);  // PlayBackground's blurred glow
        button.setBackgroundTintList(null);
        PlayBackground background = new PlayBackground(getResources().getDisplayMetrics().density, primary, 6, false);
        button.setBackground(background);
        int side = Math.round(background.inset() + background.lean(dp(52))) + dp(14);
        button.setPadding(side, 0, side, 0);
        button.setTextColor(primary ? Color.WHITE : 0xFFD1D1D6);
        button.setElevation(0);
        Menu.pressable(button);
        return button;
    }

    /** Extra game options for tests (GameOptions.EXTRA_ARGS): kept as typed, filtered when the game starts. */
    private void editExtraOptions() {
        android.widget.EditText field = new android.widget.EditText(this);
        field.setText(GameOptions.get(this, GameOptions.EXTRA_ARGS));
        field.setHint("--name=value --other=value");
        field.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        field.setTextColor(Menu.PRIMARY);
        field.setBackgroundTintList(ColorStateList.valueOf(Menu.ACCENT));
        field.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        LinearLayout box = ui.column();
        box.setPadding(dp(24), dp(18), dp(24), dp(8));
        box.addView(ui.text(tr(R.string.extra_game_options_description), 13.5f, 0xFFB4B4BC, false));
        box.addView(field);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(tr(R.string.extra_game_options))
                .setView(box)
                .setNegativeButton(tr(R.string.cancel), null)
                .setPositiveButton(tr(R.string.save_name), (d, w) ->
                        GameOptions.set(this, GameOptions.EXTRA_ARGS, field.getText().toString().trim()))
                .show();
    }

    /** The player's name: what the home page and the game show (GameOptions.cleanGamertag). */
    private void editPlayerName() {
        android.widget.EditText field = new android.widget.EditText(this);
        field.setSingleLine(true);
        field.setText(GameOptions.gamertag(this));
        field.setSelectAllOnFocus(true);
        field.setFilters(new android.text.InputFilter[] {
                new android.text.InputFilter.LengthFilter(GameOptions.GAMERTAG_MAX),
                (source, start, end, dest, dstart, dend) -> {
                    // Letters, digits and spaces only, as typed: the rest is dropped.
                    StringBuilder kept = new StringBuilder();
                    for (int i = start; i < end; i++) {
                        char c = source.charAt(i);
                        if (c == ' ' || (c < 128 && Character.isLetterOrDigit(c))) kept.append(c);
                    }
                    return kept.length() == end - start ? null : kept;
                }});
        field.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        field.setTextColor(Menu.PRIMARY);
        field.setBackgroundTintList(ColorStateList.valueOf(Menu.ACCENT));

        // The home page's name card, larger: the same slanted see-through box with the pink line at its bottom left,
        // and the buttons as plates that darken when pressed, as Play does.
        LinearLayout box = ui.column();
        box.setBackground(new NeonShapes.FadedBorderCard(0xE6141418, dp(16), getResources().getDisplayMetrics().density));
        box.setPadding(dp(64), dp(22), dp(64), dp(16));  // clear of its slanted sides
        TextView title = ui.text(tr(R.string.edit_player_name), 20, Menu.PRIMARY, true);
        box.addView(title);
        TextView rules = ui.text(tr(R.string.player_name_rules), 13.5f, 0xFFB4B4BC, false);
        rules.setLineSpacing(0, 1.12f);
        rules.setPadding(0, dp(6), 0, dp(8));
        box.addView(rules);
        box.addView(field);
        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        MaterialButton cancel = dialogButton(tr(R.string.cancel), false);
        MaterialButton save = dialogButton(tr(R.string.save_name), true);
        buttons.addView(cancel, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(52)));
        LinearLayout.LayoutParams saveParams =
                new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(52));
        saveParams.leftMargin = dp(4);
        buttons.addView(save, saveParams);
        LinearLayout.LayoutParams buttonsParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        buttonsParams.topMargin = dp(10);
        box.addView(buttons, buttonsParams);

        AlertDialog dialog = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setView(box)
                .create();
        cancel.setOnClickListener(v -> dialog.dismiss());
        // Kept open on an empty name: says why instead of closing.
        save.setOnClickListener(v -> {
            String name = GameOptions.cleanGamertag(field.getText().toString());
            if (name.isEmpty()) {
                field.setError(tr(R.string.player_name_empty));
                return;
            }
            GameOptions.set(this, GameOptions.GAMERTAG, name);
            homeName.setText(name);
            dialog.dismiss();
        });
        dialog.setOnShowListener(d -> field.requestFocus());
        if (dialog.getWindow() != null) {
            dialog.getWindow().setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
            dialog.getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(Color.TRANSPARENT));
        }
        dialog.show();
        if (dialog.getWindow() != null) {
            DisplayMetrics metrics = getResources().getDisplayMetrics();
            dialog.getWindow().setLayout(Math.min(dp(500), metrics.widthPixels - dp(32)),
                    android.view.ViewGroup.LayoutParams.WRAP_CONTENT);
        }
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
                new String[] {tr(R.string.emulated), tr(R.string.native_recommended)});
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
            rebuildPage(PERFORMANCE);  // its rows follow the renderer too
        });
        return pageScroll(ui.pageTitle(pageName(GRAPHICS),
                        tr(R.string.pick_the_renderer_the_options_below)),
                ui.header(tr(R.string.renderer)),
                ui.group(ui.stackedRow(tr(R.string.how_the_game_is_drawn), null, renderer)),
                rendererNote,
                rendererOptions,
                ui.header(tr(R.string.world_detail)),
                ui.group(
                        worldFactorRow(GameOptions.WORLD_GRASS, tr(R.string.world_grass),
                                tr(R.string.world_grass_description)),
                        ui.toggleRow(tr(R.string.foliage_coverage), tr(R.string.foliage_coverage_description),
                                GameOptions.flag(this, GameOptions.FOLIAGE_COVERAGE),
                                v -> GameOptions.flag(this, GameOptions.FOLIAGE_COVERAGE, v)),
                        worldFactorRow(GameOptions.WORLD_OBJECTS, tr(R.string.world_objects),
                                tr(R.string.world_objects_description)),
                        worldFactorRow(GameOptions.WORLD_REFLECTIONS, tr(R.string.world_reflections),
                                tr(R.string.world_reflections_description)),
                        worldFactorRow(GameOptions.WORLD_SHADOWS, tr(R.string.world_shadows),
                                tr(R.string.world_shadows_description)),
                        worldFactorRow(GameOptions.WORLD_CROWD, tr(R.string.world_crowd),
                                tr(R.string.world_crowd_description))),
                ui.footer(tr(R.string.world_detail_footer)),
                restoreDefaultsGroup());
    }

    /**
     * A world detail factor (app/src/fh2_mundo.cpp): a slider over GameOptions.worldFactors(key), 0.25x to 4x of the
     * Xbox 360's, grass from off.
     */
    private LinearLayout worldFactorRow(String key, String title, String description) {
        LinearLayout box = ui.column();
        TextView readout = ui.text("", 22, Menu.PRIMARY, true);
        box.addView(readout);
        float[] factors = GameOptions.worldFactors(key);
        int chosen = 0;
        float current = GameOptions.worldFactor(this, key);
        for (int i = 0; i < factors.length; i++) {
            if (factors[i] == current) chosen = i;
        }
        Slider slider = ui.slider(0, factors.length - 1, 1, chosen);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(2);
        box.addView(slider, lp);
        Runnable show = () -> {
            float value = GameOptions.worldFactor(this, key);
            String text = (value == Math.round(value) ? String.valueOf(Math.round(value)) : String.valueOf(value)) + "x";
            // "Reflexos" is masculine in Portuguese and Spanish; grass, shadow distance and crowd are feminine.
            String off = GameOptions.WORLD_REFLECTIONS.equals(key) ? tr(R.string.off_masculine) : tr(R.string.off_feminine);
            readout.setText(value == 0f ? off
                    : value == 1f ? text + tr(R.string.as_on_the_xbox_360) : text);
        };
        slider.addOnChangeListener((s, value, fromUser) -> {
            if (!fromUser) return;
            int index = Math.max(0, Math.min(factors.length - 1, Math.round(value)));
            GameOptions.worldFactor(this, key, factors[index]);
            show.run();
        });
        show.run();
        return ui.stackedRow(title, description, box);
    }

    // The options the game does not apply yet (shadows, shadow distance, car reflections, GPU turbo): kept, hidden
    // until it does.
    private static final boolean SHOW_NOT_APPLIED = false;

    /** At the foot of the Graphics and Performance pages: both pages' options back to their defaults, once confirmed. */
    private View restoreDefaultsGroup() {
        LinearLayout column = ui.column();
        column.setPadding(0, dp(22), 0, 0);
        column.addView(ui.group(ui.actionRow(tr(R.string.restore_defaults), tr(R.string.restore_defaults_description),
                true, v -> new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                        .setTitle(tr(R.string.restore_defaults_question))
                        .setMessage(tr(R.string.restore_defaults_message))
                        .setPositiveButton(tr(R.string.restore), (d, w) -> {
                            GameOptions.restoreDefaults(this);
                            rebuildPage(GRAPHICS);
                            rebuildPage(PERFORMANCE);
                            toast(tr(R.string.defaults_restored));
                        })
                        .setNegativeButton(tr(R.string.cancel), null)
                        .show())));
        return column;
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
                ? tr(R.string.native_experimental_draws_the_game_s)
                : tr(R.string.emulated_the_xbox_360_gpu_emulated);
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
            rendererOptions.addView(ui.header(tr(R.string.requirement)));
            rendererOptions.addView(ui.group(ui.statusRow(tr(R.string.shader_library),
                    tr(R.string.fh2_shaders_nfsp_next_to_the, GAME_FOLDER_NAME),
                    library, library ? tr(R.string.found) : tr(R.string.missing_2))));

            rendererOptions.addView(ui.header(tr(R.string.native_image)));
            SegmentedControl aa = antiAliasingControl(null);
            rendererOptions.addView(ui.group(
                    renderScaleRow(true, null),
                    ui.stackedRow(tr(R.string.anti_aliasing),
                            tr(R.string.fxaa_smooths_jagged_edges_with_a), aa),
                    anisotropicRow(),
                    shadowMapRow(),
                    scalingRow(),
                    fsrModeRow(),
                    sharpnessRow()));
            rendererOptions.addView(ui.footer(tr(R.string.msaa_2x_4x_draws_the_3d)));

            rendererOptions.addView(ui.header(tr(R.string.native_performance)));
            LinearLayout onePass = onePassRow(
                    tr(R.string.without_msaa_the_scene_fits_whole),
                    // B19: in 3 tiles the native renderer only shows the bottom one (black and grey bands above).
                    tr(R.string.turned_off_it_breaks_the_game));
            onePass.setVisibility(View.GONE);  // off breaks the native renderer: kept on, out of the player's way
            LinearLayout performance = ui.group(fpsLimitRow(), onePass,
                    ui.toggleRow(tr(R.string.deferred_scene_copy),
                            tr(R.string.experimental_the_copy_of_the_finished),
                            GameOptions.flag(this, GameOptions.LAZY_COMPOSITE),
                            v -> GameOptions.flag(this, GameOptions.LAZY_COMPOSITE, v)),
                    ui.toggleRow(tr(R.string.step_pacing),
                            tr(R.string.step_pacing_description),
                            GameOptions.flag(this, GameOptions.STEP_PACING),
                            v -> GameOptions.flag(this, GameOptions.STEP_PACING, v)));
            Menu.refreshDividers(performance);
            rendererOptions.addView(performance);
        } else {
            rendererOptions.addView(ui.header(tr(R.string.emulated_image)));
            LinearLayout image = ui.group();
            Runnable[] refreshScale = new Runnable[1];
            LinearLayout fxaa = ui.choiceRow(GameOptions.FXAA_QUALITY, tr(R.string.fxaa_quality),
                    tr(R.string.high_smooths_more_edges_the_faint),
                    new String[] {"Normal", tr(R.string.high_2)}, new String[] {"normal", "high"});
            Runnable showModes = () -> {
                fxaa.setVisibility("fxaa".equals(GameOptions.get(this, GameOptions.ANTI_ALIASING))
                        ? View.VISIBLE : View.GONE);
                Menu.refreshDividers(image);
            };
            image.addView(renderScaleRow(false, refreshScale));
            image.addView(ui.stackedRow(tr(R.string.anti_aliasing),
                    tr(R.string.smooths_jagged_edges_msaa_is_the),
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

            rendererOptions.addView(ui.header(tr(R.string.emulated_performance)));
            rendererOptions.addView(ui.group(
                    fpsLimitRow(),
                    onePassRow(tr(R.string.the_game_draws_the_scene_in)),
                    ui.toggleRow(tr(R.string.direct_copies_to_textures),
                            tr(R.string.experimental_when_the_game_copies_an),
                            GameOptions.flag(this, GameOptions.RESOLVE_SHORTCUT),
                            v -> GameOptions.flag(this, GameOptions.RESOLVE_SHORTCUT, v)),
                    ui.toggleRow(tr(R.string.compile_shaders_in_the_background),
                            tr(R.string.off_the_game_waits_for_each),
                            GameOptions.flag(this, GameOptions.ASYNC_SHADERS),
                            v -> GameOptions.flag(this, GameOptions.ASYNC_SHADERS, v))));
        }
        if (!SHOW_NOT_APPLIED) return;
        rendererOptions.addView(ui.header(tr(R.string.not_applied_yet)));
        rendererOptions.addView(ui.group(
                ui.choiceRow(GameOptions.SHADOWS, tr(R.string.shadows),
                        tr(R.string.how_often_the_shadow_maps_are),
                        new String[] {tr(R.string.every_frame), tr(R.string.every_2)},
                        new String[] {"1", "2"}),
                ui.choiceRow(GameOptions.SHADOW_DISTANCE, tr(R.string.shadow_distance),
                        tr(R.string.how_far_objects_cast_shadows_nearly),
                        new String[] {tr(R.string.far), tr(R.string.medium_2), tr(R.string.near)},
                        new String[] {"far", "medium", "near"}),
                ui.choiceRow(GameOptions.CAR_REFLECTIONS, tr(R.string.car_reflections),
                        tr(R.string.faces_of_the_car_s_reflection),
                        new String[] {tr(R.string.high), tr(R.string.medium), tr(R.string.low)},
                        new String[] {"high", "medium", "low"})));
        rendererOptions.addView(ui.footer(tr(R.string.they_are_stored_but_the_game)));
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
        return ui.choiceRow(GameOptions.FPS, tr(R.string.fps_limit),
                tr(R.string.the_game_s_cap_30_is),
                new String[] {"30", "60", "90", "120", tr(R.string.free)},
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
                percent.setText(tr(R.string.off_plural));
                size.setText("");
                return;
            }
            // Same rounding as Fh2Escalar (nfsmw_nativo_destinos.cpp): the 1040 target in multiples of 16, at least
            // 64; the 1024x1024 the game resolves scales with it.
            int side = value >= 100 ? 1024 : Math.max(64, ((1040 * value / 100) + 15) & ~15) * 1024 / 1040;
            percent.setText(value + " %");
            size.setText(side + "×" + side + (value >= 100 ? tr(R.string.as_on_the_xbox_360) : ""));
        };
        slider.addOnChangeListener((s, value, fromUser) -> {
            if (!fromUser) return;
            GameOptions.set(this, GameOptions.SHADOW_MAP_SCALE, String.valueOf(Math.round(value)));
            show.run();
        });
        show.run();
        return ui.stackedRow(tr(R.string.shadow_quality),
                tr(R.string.size_of_the_sun_s_shadow), box);
    }

    private LinearLayout anisotropicRow() {
        return ui.choiceRow(GameOptions.ANISOTROPIC, tr(R.string.anisotropic_filtering),
                tr(R.string.sharper_textures_seen_at_an_angle),
                new String[] {tr(R.string.off_no), "2x", "4x", "16x"}, new String[] {"0", "2", "3", "5"});
    }

    private LinearLayout onePassRow(String description) {
        return onePassRow(description, null);
    }

    /** With a warning, it follows the description in bold. */
    private LinearLayout onePassRow(String description, String warning) {
        LinearLayout row = ui.toggleRow(tr(R.string.scene_in_one_pass), description,
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
        SegmentedControl aa = new SegmentedControl(this, new String[] {tr(R.string.off_feminine), "FXAA",
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
        return ui.choiceRow(GameOptions.SCALING, tr(R.string.scaling_to_the_screen),
                tr(R.string.how_the_image_is_brought_to),
                new String[] {tr(R.string.off_masculine), "Bilinear", "FSR 1.0", "CAS"},
                new String[] {"off", "bilinear", "fsr", "cas"},
                value -> refreshFsrModeRows());
    }

    private View fsrModeRow() {
        LinearLayout row = ui.choiceRow(GameOptions.FSR_MODE, tr(R.string.fsr_mode),
                tr(R.string.shortcut_render_scale_fsr_1_0),
                new String[] {tr(R.string.mega_quality), tr(R.string.ultra_quality),
                        tr(R.string.quality), tr(R.string.balanced), tr(R.string.performance),
                        tr(R.string.ultra_performance)},
                new String[] {"mega_quality", "ultra_quality", "quality", "balanced", "performance", "ultra_performance"});
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
        return ui.stackedRow(tr(R.string.extra_sharpness_cas_fsr),
                tr(R.string.extra_sharpness_on_top_of_cas),
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

        String original = tr(R.string.the_original_resolution);
        String comma = tr(R.string.text_2);
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
        return ui.stackedRow(tr(R.string.render_resolution), nativeRenderer
                ? tr(R.string.how_much_of_the_xbox_360_2)
                : tr(R.string.how_much_of_the_xbox_360), box);
    }

    // ---- Display, performance, sound -----------------------------------------------------------------------

    private ScrollView displayPage() {
        boolean hdrSupported = GameOptions.hdrCapabilities(this) != null;
        // Relative HDR (Android 15+): the white follows the brightness slider, so there is no white to choose.
        boolean hdrRelative = hdrSupported && GameOptions.hdrRelative(this);
        return pageScroll(ui.pageTitle(pageName(DISPLAY),
                        tr(R.string.the_final_image_and_what_shows)),
                ui.header(tr(R.string.image)),
                ui.group(resolutionRow(),
                        ui.toggleRow(tr(R.string.stretch_to_the_whole_screen),
                                tr(R.string.off_the_xbox_360_s_16),
                                GameOptions.stretch(this), v -> GameOptions.stretch(this, v))),
                ui.header(tr(R.string.presentation)),
                ui.group(ui.choiceRow(GameOptions.PRESENT_MODE, tr(R.string.present_mode),
                                tr(R.string.fifo_waits_for_the_screen_s),
                                new String[] {"FIFO · vsync", "MAILBOX"}, new String[] {"fifo", "mailbox"}),
                        ui.toggleRow(tr(R.string.adaptive_vblank),
                                tr(R.string.a_frame_that_misses_the_vsync),
                                GameOptions.flag(this, GameOptions.ADAPTIVE_VBLANK),
                                v -> GameOptions.flag(this, GameOptions.ADAPTIVE_VBLANK, v))),
                // Only where the display reports HDR10 (GameOptions.hdrCapabilities).
                hdrSupported ? ui.header(tr(R.string.hdr)) : null,
                hdrSupported ? ui.group(ui.toggleRow(tr(R.string.hdr_output),
                                tr(hdrRelative ? R.string.the_screen_s_hdr_mode_the_white
                                        : R.string.the_screen_s_hdr_mode_with),
                                GameOptions.flag(this, GameOptions.HDR),
                                v -> GameOptions.flag(this, GameOptions.HDR, v)),
                        hdrRelative ? null : ui.choiceRow(GameOptions.HDR_PAPER_WHITE, tr(R.string.hdr_white),
                                tr(R.string.how_bright_white_menus_and_the),
                                GameOptions.HDR_PAPER_WHITES, GameOptions.HDR_PAPER_WHITES),
                        ui.choiceRow(GameOptions.HDR_HIGHLIGHTS, tr(R.string.hdr_highlights),
                                tr(R.string.how_far_the_scene_s_brightest),
                                new String[] {tr(R.string.off_masculine), tr(R.string.low), tr(R.string.medium),
                                        tr(R.string.high)},
                                GameOptions.HDR_HIGHLIGHT_LEVELS),
                        // HDR10's declared peak; the relative mode takes the display's own headroom instead.
                        hdrRelative ? null : ui.choiceRow(GameOptions.HDR_PEAK, tr(R.string.hdr_peak),
                                tr(R.string.how_bright_the_highlights_may_get),
                                new String[] {tr(R.string.hdr_peak_auto),
                                        tr(R.string.hdr_peak_reported) + " (" +
                                                Math.round(GameOptions.hdrCapabilities(this)[0]) + ")",
                                        "1500"},
                                GameOptions.HDR_PEAKS)) : null,
                ui.header(tr(R.string.overlay)),
                ui.group(ui.toggleRow(tr(R.string.show_fps),
                                tr(R.string.a_counter_in_the_corner_frames),
                                GameOptions.flag(this, GameOptions.SHOW_FPS),
                                v -> GameOptions.flag(this, GameOptions.SHOW_FPS, v)),
                        ui.toggleRow(tr(R.string.show_gpu_cpu_and_ram_use),
                                tr(R.string.under_the_fps_gpu_load_the),
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
        labels.add(tr(R.string.native_resolution));
        values.add(GameOptions.RES_NATIVE);
        return ui.choiceRow(GameOptions.RESOLUTION, tr(R.string.resolution),
                tr(R.string.size_of_the_final_image_on, longSide, shortSide),
                labels.toArray(new String[0]), values.toArray(new String[0]));
    }

    private ScrollView performancePage() {
        boolean adreno = GpuDrivers.isAdreno(this);
        return pageScroll(ui.pageTitle(pageName(PERFORMANCE),
                        tr(R.string.the_processor_and_the_gpu_driver)),
                ui.header(tr(R.string.processor)),
                ui.group(processorRows()),
                ui.header(tr(R.string.gpu_driver)),
                driverGroup(),
                ui.footer(adreno
                        ? tr(R.string.import_an_android_driver_s_zip)
                        : tr(R.string.turnip_and_qualcomm_drivers_are_for)),
                ui.header(tr(R.string.vulkan_test)),
                probeGroup(),
                ui.footer(tr(R.string.opens_the_driver_the_way_the)),
                SHOW_NOT_APPLIED ? ui.header(tr(R.string.not_applied_yet)) : null,
                SHOW_NOT_APPLIED ? turboGroup() : null,
                restoreDefaultsGroup());
    }

    private String chosenDriver() {
        String chosen = GameOptions.get(this, GameOptions.DRIVER);
        return GpuDrivers.find(this, chosen) == null ? "" : chosen;
    }

    private static String driverTitle(String name) {
        return name.isEmpty() ? tr(R.string.system_driver) : name;
    }

    // The processor rows of both renderers, then the native renderer's own only when it is the chosen one.
    private View[] processorRows() {
        boolean nativeRenderer = GameOptions.nativeRenderer(this);
        List<View> rows = new ArrayList<>();
        // First on the page, where a tester finds it.
        rows.add(ui.actionRow(tr(R.string.extra_game_options), tr(R.string.extra_game_options_description), false,
                v -> editExtraOptions()));
        rows.add(ui.toggleRow(tr(R.string.pin_threads_to_cores),
                tr(R.string.keeps_the_game_on_the_processor),
                GameOptions.flag(this, GameOptions.PIN_THREADS),
                v -> GameOptions.flag(this, GameOptions.PIN_THREADS, v)));
        rows.add(ui.toggleRow(tr(R.string.fastest_core_for_the_game),
                tr(R.string.instead_of_the_ring_gives_the),
                GameOptions.flag(this, GameOptions.JOGO_RAPIDO),
                v -> GameOptions.flag(this, GameOptions.JOGO_RAPIDO, v)));
        rows.add(ui.toggleRow(tr(R.string.present_thread),
                tr(R.string.present_thread_desc),
                GameOptions.flag(this, GameOptions.PRESENT_THREAD),
                v -> GameOptions.flag(this, GameOptions.PRESENT_THREAD, v)));
        if (nativeRenderer) {
            rows.add(ui.toggleRow(tr(R.string.vertex_cache_across_frames),
                    tr(R.string.reuses_the_vertices_the_game_has),
                    GameOptions.flag(this, GameOptions.VERTEX_CACHE),
                    v -> GameOptions.flag(this, GameOptions.VERTEX_CACHE, v)));
            rows.add(ui.toggleRow(tr(R.string.index_cache_across_frames),
                    tr(R.string.reuses_the_already_converted_indices_the),
                    GameOptions.flag(this, GameOptions.INDEX_CACHE),
                    v -> GameOptions.flag(this, GameOptions.INDEX_CACHE, v)));
            rows.add(ui.toggleRow(tr(R.string.vertices_without_waiting_between_passes),
                    tr(R.string.the_gpu_starts_a_pass_s),
                    GameOptions.flag(this, GameOptions.VERTEX_NO_WAIT),
                    v -> GameOptions.flag(this, GameOptions.VERTEX_NO_WAIT, v)));
            rows.add(ui.toggleRow(tr(R.string.small_targets_at_exact_size),
                    tr(R.string.creates_the_small_render_targets_reflect),
                    GameOptions.flag(this, GameOptions.EXACT_TARGETS),
                    v -> GameOptions.flag(this, GameOptions.EXACT_TARGETS, v)));
            rows.add(ui.toggleRow(tr(R.string.reflection_mips_in_one_pass),
                    tr(R.string.draws_the_reflection_mips_level_by_level),
                    GameOptions.flag(this, GameOptions.REFLECTION_MIPS),
                    v -> GameOptions.flag(this, GameOptions.REFLECTION_MIPS, v)));
            rows.add(ui.toggleRow(tr(R.string.reflection_stagger),
                    tr(R.string.reflection_stagger_description),
                    GameOptions.flag(this, GameOptions.REFLECTION_STAGGER),
                    v -> GameOptions.flag(this, GameOptions.REFLECTION_STAGGER, v)));
            rows.add(ui.toggleRow(tr(R.string.tight_pass_area),
                    tr(R.string.tight_pass_area_description),
                    GameOptions.flag(this, GameOptions.TIGHT_AREA),
                    v -> GameOptions.flag(this, GameOptions.TIGHT_AREA, v)));
        }
        if (GameOptions.PERFORMANCE_HINTS_IN_MENU) {
            rows.add(ui.toggleRow(tr(R.string.performance_hints_adpf),
                    tr(R.string.tells_android_how_long_each_frame),
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
        group.addView(ui.actionRow(tr(R.string.import_a_driver_zip), null, false, v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("application/zip");
            startActivityForResult(pick, REQUEST_DRIVER_ZIP);
        }));
        LinearLayout delete = ui.actionRow(tr(R.string.delete_the_chosen_one), null, true, v -> {
            GpuDrivers.Driver driver = GpuDrivers.find(this, GameOptions.get(this, GameOptions.DRIVER));
            if (driver == null) return;
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(tr(R.string.delete_2, driver.name))
                    .setMessage(tr(R.string.the_driver_leaves_the_phone_and))
                    .setPositiveButton(tr(R.string.delete), (d, w) -> {
                        GpuDrivers.remove(this, driver);
                        GameOptions.set(this, GameOptions.DRIVER, "");
                        rebuildPage(PERFORMANCE);
                    })
                    .setNegativeButton(tr(R.string.cancel), null)
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
        if (name.equals(probingDriver)) state = tr(R.string.testing_2);
        else if (probe == null) state = tr(R.string.not_tested);
        else if (probePassed(probe)) state = "Vulkan " + probe.optString("vulkan", "?") + " · " + tr(R.string.ok);
        else state = tr(R.string.failed_the_test);
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
            status = ui.statusRow(driverTitle(name), tr(R.string.opening_the_driver_and_creating_a), Menu.AMBER, tr(R.string.testing));
        } else if (probe == null) {
            status = ui.row(driverTitle(name), tr(R.string.not_tested_yet), null);
        } else {
            boolean passed = probePassed(probe);
            status = ui.statusRow(driverTitle(name), passed
                            ? tr(R.string.the_game_can_use_this_driver)
                            : tr(R.string.the_game_may_not_start_with_2),
                    passed, passed ? tr(R.string.compatible) : tr(R.string.failed_2));
        }
        group.addView(status);
        if (probe != null) {
            String driverName = probe.optString("driverName", "");
            String driverInfo = probe.optString("driverInfo", "");
            if (probe.has("gpu")) group.addView(detailRow("GPU", probe.optString("gpu")));
            if (!driverName.isEmpty() || !driverInfo.isEmpty()) {
                group.addView(detailRow(tr(R.string.driver),
                        (driverName + (driverInfo.isEmpty() ? "" : " · " + driverInfo)).trim()));
            }
            if (probe.has("vulkan")) group.addView(detailRow("Vulkan", probe.optString("vulkan")));
            if (probe.has("logicalDeviceResult")) {
                int result = probe.optInt("logicalDeviceResult");
                group.addView(detailRow(tr(R.string.logical_device), result == 0
                        ? tr(R.string.created) : tr(R.string.failed) + " (VkResult " + result + ")"));
            }
            JSONArray missing = probe.optJSONArray("missing");
            if (missing != null && missing.length() > 0) {
                StringBuilder list = new StringBuilder();
                for (int i = 0; i < missing.length(); i++) {
                    if (i > 0) list.append('\n');
                    list.append("• ").append(missing.optString(i));
                }
                group.addView(detailRow(tr(R.string.missing), list.toString()));
            }
            if (probe.optBoolean("driverNotLoaded")) group.addView(detailRow(tr(R.string.error), notLoaded()));
            if (probe.has("probeError")) group.addView(detailRow(tr(R.string.error), probe.optString("probeError")));
        }
        LinearLayout again = ui.actionRow(probe == null && probingDriver == null
                ? tr(R.string.test_the_driver) : tr(R.string.test_again), null, false,
                v -> testDriver(chosenDriver(), false));
        Menu.setRowEnabled(again, probingDriver == null);
        group.addView(again);
        Menu.refreshDividers(group);
        return group;
    }

    private static String notLoaded() {
        return tr(R.string.the_imported_driver_did_not_load);
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
                    JSONObject.quote(tr(R.string.the_driver_crashed_the_test_process)) + "}";
        }
        GpuDrivers.saveProbe(this, name, report);
        android.util.Log.i("FH2", "Vulkan test of " + driverTitle(name) + ": " + report);
        JSONObject probe = GpuDrivers.lastProbe(this, name);
        boolean passed = probe != null && probePassed(probe);
        if (choose && passed) {
            GameOptions.set(this, GameOptions.DRIVER, name);
            toast(tr(R.string.driver_chosen) + driverTitle(name));
        } else if (choose) {
            String reason = probe == null ? "" : probe.optBoolean("driverNotLoaded") ? notLoaded()
                    : probe.optString("probeError", "");
            JSONArray missing = probe == null ? null : probe.optJSONArray("missing");
            if (reason.isEmpty() && missing != null && missing.length() > 0) reason = missing.optString(0);
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(tr(R.string.the_driver_failed_the_test))
                    .setMessage(driverTitle(name) + (reason.isEmpty() ? "" : "\n\n" + reason) + "\n\n" +
                            tr(R.string.the_game_may_not_start_with))
                    .setPositiveButton(tr(R.string.use_it_anyway), (d, w) -> {
                        GameOptions.set(this, GameOptions.DRIVER, name);
                        rebuildPage(PERFORMANCE);
                    })
                    .setNegativeButton(tr(R.string.cancel), null)
                    .show();
        }
        rebuildPage(PERFORMANCE);
    }

    // The turbo locks the GPU clock for the whole phone: only with a Turnip chosen, as in the fork.
    private LinearLayout turboGroup() {
        GpuDrivers.Driver selected = GpuDrivers.find(this, GameOptions.get(this, GameOptions.DRIVER));
        boolean turnip = selected != null && selected.isTurnip();
        LinearLayout turbo = ui.toggleRow(tr(R.string.gpu_turbo),
                tr(R.string.locks_the_gpu_clock_instead_of),
                turnip && GameOptions.flag(this, GameOptions.TURBO), v -> GameOptions.flag(this, GameOptions.TURBO, v));
        Menu.setRowEnabled(turbo, turnip);
        return ui.group(turbo);
    }

    private ScrollView soundPage() {
        // Volume group: choice row followed by its footer note.
        LinearLayout volumeBox = ui.group(
                ui.toggleRow(tr(R.string.decode_audio_in_the_background),
                        tr(R.string.on_a_thread_of_its_own),
                        GameOptions.flag(this, GameOptions.AUDIO_WORKER),
                        v -> GameOptions.flag(this, GameOptions.AUDIO_WORKER, v)),
                ui.choiceRow(GameOptions.VOLUME, tr(R.string.game_volume), null,
                        new String[] {"Normal", tr(R.string.loud), tr(R.string.very_loud), tr(R.string.max)},
                        new String[] {"100", "125", "150", "200"}));
        TextView volumeFooter = ui.footer(tr(R.string.above_normal_the_sound_may_distort));

        // Vibration section: toggle row + mode selector (shown only when vibration is on).
        LinearLayout vibrationModeRow = ui.choiceRow(GameOptions.VIBRATION_MODE, tr(R.string.vibration_mode),
                tr(R.string.choose_what_vibrates_when_the_game),
                new String[] {tr(R.string.phone_only),
                              tr(R.string.gamepad_only),
                              tr(R.string.phone_and_gamepad)},
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
        LinearLayout vibrationToggle = ui.toggleRow(tr(R.string.vibration),
                tr(R.string.makes_the_phone_vibrate_when_the),
                GameOptions.flag(this, GameOptions.VIBRATION_PHONE),
                v -> {
                    GameOptions.flag(this, GameOptions.VIBRATION_PHONE, v);
                    vibrationModeRow.setVisibility(v ? View.VISIBLE : View.GONE);
                });
        vibrationBox.addView(vibrationToggle);
        vibrationBox.addView(vibrationModeRow);

        return pageScroll(ui.pageTitle(pageName(SOUND), null),
                ui.header(tr(R.string.volume)),
                volumeBox,
                volumeFooter,
                ui.header(tr(R.string.vibration)),
                vibrationBox);
    }

    // ---- Achievements: the game's, unlocked ones in the app's magenta, locked ones grey with a lock ------------

    private ScrollView achievementsPage() {
        List<Achievements.Entry> all = Achievements.load(this);
        if (all.isEmpty()) {
            return pageScroll(ui.pageTitle(pageName(ACHIEVEMENTS), null),
                    ui.group(ui.row(tr(R.string.no_achievements_yet),
                            tr(R.string.the_list_comes_from_the_game),
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
            parts.add(ui.header(tr(R.string.unlocked_2)));
            List<View> rows = new ArrayList<>();
            for (Achievements.Entry e : unlocked) rows.add(achievementRow(e));
            parts.add(ui.group(rows.toArray(new View[0])));
        }
        if (!locked.isEmpty()) {
            parts.add(ui.header(tr(R.string.locked)));
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
        TextView of = ui.text(tr(R.string.unlocked), 14, Menu.SECONDARY, false);
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
        labels.addView(ui.text(hidden ? tr(R.string.secret_achievement) : e.label, 15.5f,
                got ? Menu.PRIMARY : Menu.SECONDARY, got));
        String text = got ? e.description : hidden
                ? tr(R.string.keep_playing_to_find_out) : e.lockedDescription;
        if (text != null && !text.isEmpty()) {
            TextView d = ui.text(text, 13, got ? Menu.SECONDARY : Menu.TERTIARY, false);
            d.setLineSpacing(0, 1.1f);
            d.setPadding(0, dp(2), 0, 0);
            labels.addView(d);
        }
        if (got) {
            String day = java.text.DateFormat.getDateInstance(java.text.DateFormat.MEDIUM,
                    AppLanguage.locale())
                    .format(new java.util.Date(e.unlockedAtMillis));
            TextView when = ui.text(tr(R.string.unlocked_on) + day, 12, Menu.ACCENT_TEXT, false);
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
            String what = tr(language.description);
            if (language.voiceOver) what += tr(R.string.voiced);
            game.addView(ui.checkRow(language.name, what, language.code.equals(chosen), v -> {
                GameOptions.set(this, GameOptions.GAME_LANGUAGE, language.code);
                rebuildPage(LANGUAGE);
            }));
        }
        Menu.refreshDividers(game);

        String[] names = AppLanguage.NAMES.clone();
        names[0] = tr(R.string.app_language_system);
        LinearLayout app = ui.dropdownRow(tr(R.string.app_language),
                tr(R.string.this_menu_and_the_touch_controls),
                names, AppLanguage.chosen(this), index -> {
                    GameOptions.set(this, GameOptions.APP_LANGUAGE, AppLanguage.CODES[index]);
                    recreate();  // attachBaseContext again, in the new language; the page is in the saved state
                });

        return pageScroll(ui.pageTitle(pageName(LANGUAGE),
                        tr(R.string.the_game_s_language_and_this)),
                ui.header(tr(R.string.app)),
                ui.group(app),
                ui.header(tr(R.string.game)),
                game,
                ui.footer(tr(R.string.every_language_on_the_disc_texts)));
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
        selectFolder = ui.actionRow(tr(R.string.choose_the_game_folder), null, false,
                view -> withStorageAccess(this::selectGameFolder));

        selectIso = ui.actionRow(tr(R.string.use_the_game_iso),
                tr(R.string.read_straight_from_the_file_no),
                false, view -> withStorageAccess(this::selectIsoFile));
        stopIso = ui.actionRow(tr(R.string.stop_using_the_iso),
                tr(R.string.back_to_the_extracted_folder), true, view -> {
                    GameOptions.set(this, GameOptions.GAME_ISO, "");
                    showGameFolder(null);
                });

        LinearLayout exportSave = ui.actionRow(tr(R.string.export_the_save),
                tr(R.string.to_jogos) + GAME_FOLDER_NAME + "-saves-exportados", false, v -> withStorageAccess(() -> {
                    try {
                        File target = SaveTransfer.export(this);
                        toast(tr(R.string.save_exported_to_jogos) +
                                target.getParentFile().getName() + "/" + target.getName());
                    } catch (IOException e) {
                        toast(tr(R.string.could_not_export) + e.getMessage());
                    }
                }));
        LinearLayout importSave = ui.actionRow(tr(R.string.import_a_save),
                tr(R.string.from_an_export_or_an_automatic),
                false, v -> withStorageAccess(this::chooseSaveToImport));
        return pageScroll(ui.pageTitle(pageName(GAME),
                        tr(R.string.the_game_files_and_your_progress)),
                ui.header(tr(R.string.game_folder)),
                ui.group(status, selectIso, selectFolder, stopIso),
ui.footer(tr(R.string.a_disc_image_the_xbox_360, GAME_FOLDER_NAME)),
                ui.header("Save"),
                ui.group(exportSave, importSave),
                ui.footer(tr(R.string.export_copies_your_progress_profile_and, GAME_FOLDER_NAME)));
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
        names.addView(ui.text(tr(R.string.forza_horizon_2_for_the_xbox), 13, Menu.SECONDARY, false));
        String version = "";
        try {
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException ignored) {
        }
        TextView versionText = ui.text(tr(R.string.version) + version, 12, Menu.TERTIARY, false);
        versionText.setPadding(0, dp(4), 0, 0);
        names.addView(versionText);
        hero.addView(names, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        saveReport = ui.actionRow(reportLabel(), null, false, view -> prepareReport(false));
        rootReport = ui.actionRow(rootReportLabel(), tr(R.string.asks_for_root_magisk_kernelsu_and), false, view -> requestRootReport());
        rootProfile = ui.toggleRow(tr(R.string.record_a_performance_profile_root),
                tr(R.string.next_game_only_after_60_s),
                GameOptions.flag(this, GameOptions.ROOT_PROFILE), this::setRootProfile);
        return pageScroll(ui.pageTitle(pageName(ABOUT), null),
                ui.group(hero),
                ui.header(tr(R.string.diagnostics)),
                ui.group(saveReport,
                        ui.toggleRow(tr(R.string.measure_the_gpu_in_the_logs),
                        tr(R.string.writes_to_the_log_how_long),
                        GameOptions.flag(this, GameOptions.GPU_MEASUREMENTS),
                        v -> GameOptions.flag(this, GameOptions.GPU_MEASUREMENTS, v)),
                        ui.toggleRow(tr(R.string.verbose_log),
                        tr(R.string.writes_every_repetition_of_the_messages),
                        GameOptions.flag(this, GameOptions.VERBOSE_LOG),
                        v -> GameOptions.flag(this, GameOptions.VERBOSE_LOG, v)),
                        rootReport,
                        rootProfile),
                ui.footer(tr(R.string.logs_and_device_information_in_a)),
                ui.header(tr(R.string.project)),
                ui.group(ui.actionRow(tr(R.string.gabrick75_on_github),
                        PROJECT_URL.replace("https://", ""), false, view -> {
                    try {
                        startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(PROJECT_URL)));
                    } catch (ActivityNotFoundException e) {
                        Toast.makeText(this, PROJECT_URL, Toast.LENGTH_LONG).show();
                    }
                })));
    }

    private static String reportLabel() {
        return tr(R.string.save_diagnostics_zip);
    }

    private void importDriver(Uri zip) {
        worker.execute(() -> {
            try {
                GpuDrivers.Driver driver = GpuDrivers.importZip(this, zip);
                mainHandler.post(() -> {
                    // Tested at once, and chosen if it passes.
                    GpuDrivers.saveProbe(this, driver.name, null);
                    toast(tr(R.string.driver_imported) + driver.name);
                    testDriver(driver.name, true);
                });
            } catch (IOException error) {
                mainHandler.post(() -> toast(tr(R.string.could_not_import_the_driver) + error.getMessage()));
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
        return tr(R.string.this_phone_s_gpu_lacks_a);
    }

    // ---- Play ----------------------------------------------------------------------------------------------

    private void play() {
        launchGame.setEnabled(false);
        homePlay.setEnabled(false);
        Diagnostics.recordLaunch(this);
        worker.execute(() -> {
            // The edition check reads default.xex from the folder; an ISO is mounted as it is (not checked).
            GameEdition edition = GameIso.chosen(this) != null ? null : GameEdition.of(this, gameRoot());
            boolean emulatedBlocked = !GameOptions.nativeRenderer(this) && emulatedRendererUnsupported(this);
            mainHandler.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                launchGame.setEnabled(true);
                homePlay.setEnabled(true);
                if (emulatedBlocked) {
                    new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle(tr(R.string.emulated_renderer_not_supported))
                            .setMessage(emulatedUnsupportedMessage())
                            .setPositiveButton(tr(R.string.use_native), (dialog, which) -> {
                                GameOptions.set(this, GameOptions.RENDERER, "native");
                                rebuildPage(GRAPHICS);
                                rebuildPage(PERFORMANCE);
                                startActivity(new Intent(this, GameActivity.class));
                            })
                            .setNeutralButton(tr(R.string.play_anyway),
                                    (dialog, which) -> startActivity(new Intent(this, GameActivity.class)))
                            .setNegativeButton(tr(R.string.back), null)
                            .show();
                    return;
                }
                if (edition != null && !edition.supported()) {
                    new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle(tr(R.string.unsupported_game_edition))
                            .setMessage(tr(R.string.this_app_was_recompiled_from_the, GameEdition.SUPPORTED_NAME, edition.sha256.substring(0, 12), GameEdition.SUPPORTED_SHA256.substring(0, 12)))
                            .setPositiveButton(tr(R.string.back), null)
                            .setNeutralButton(tr(R.string.play_anyway),
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
        addCheck(tr(R.string.media_folder), new File(folder, "media").isDirectory());
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

    // Without file access the game page says how to get it: Android's prompt comes only when the player picks the
    // game or a save (withStorageAccess), not when the app opens.
    private void prepareGameFolder() {
        if (!hasStorageAccess()) {
            setGameReady(false);
            setImportStatus(storageNotice());
            return;
        }
        File root = gameRoot();
        if (!root.isDirectory()) root.mkdirs();
        showGameFolder(null);
    }

    private Runnable afterStorageAccess;  // what asked for file access, run once Android grants it

    /** Runs then at once with file access, otherwise after Android's prompt grants it (denied, it is dropped). */
    private void withStorageAccess(Runnable then) {
        if (hasStorageAccess()) {
            then.run();
            return;
        }
        afterStorageAccess = then;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            startActivityForResult(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())), REQUEST_STORAGE_ACCESS);
        } else {
            requestPermissions(new String[] {android.Manifest.permission.WRITE_EXTERNAL_STORAGE},
                    REQUEST_LEGACY_STORAGE);
        }
    }

    private void storageAccessAnswered() {
        Runnable then = afterStorageAccess;
        afterStorageAccess = null;
        prepareGameFolder();
        if (then != null && hasStorageAccess()) then.run();
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
        Menu.titleOf(selectIso).setText(iso != null ? tr(R.string.change_the_iso)
                : tr(R.string.use_the_game_iso));
        Menu.refreshDividers((android.view.ViewGroup) selectIso.getParent());
        if (iso != null) {
            // The game reads the disc image in place: no folder to check.
            checksList.removeAllViews();
            addCheck(iso.getName(), true);
            setImportStatus((notice == null ? "" : notice + "\n") + tr(R.string.reading_straight_from_the_iso) +
                    iso.getAbsolutePath());
            setGameReady(true);
            Menu.titleOf(selectFolder).setText(tr(R.string.use_an_extracted_folder));
            return;
        }
        showChecks(root);
        if (isValidGameFolder(root)) {
            setImportStatus((notice == null ? "" : notice + "\n") + tr(R.string.internal_storage) +
                    GAME_FOLDER_NAME);
            setGameReady(true);
            Menu.titleOf(selectFolder).setText(tr(R.string.change_the_game_folder));
        } else {
            setImportStatus(tr(R.string.choose_the_game_s_extracted_folder, GAME_FOLDER_NAME));
            setGameReady(false);
            Menu.titleOf(selectFolder).setText(tr(R.string.choose_the_game_folder));
        }
    }

    private void selectGameFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        String saved = getPreferences(MODE_PRIVATE).getString(TREE_URI, null);
        if (saved != null) {
            intent.putExtra("android.provider.extra.INITIAL_URI", Uri.parse(saved));
        }
        startActivityForResult(intent, REQUEST_GAME_FOLDER);
    }

    private void selectIsoFile() {
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
            storageAccessAnswered();
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
            setImportStatus(tr(R.string.could_not_keep_the_folder_s) +
                    error.getMessage());
            return;
        }
        getPreferences(MODE_PRIVATE).edit().putString(TREE_URI, tree.toString()).apply();
        importGameFolder(tree);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_LEGACY_STORAGE) storageAccessAnswered();
    }

    // Copies the picked folder into a staging folder and only swaps it in once it is complete and valid, so an
    // interrupted import never leaves a half game in place.
    private void importGameFolder(Uri tree) {
        Menu.setRowEnabled(selectFolder, false);
        setGameReady(false);
        launchGame.setEnabled(false);
        playStatus.setText(tr(R.string.copying_the_game));
        setImportStatus(tr(R.string.copying_the_game_to_internal_storage) +
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
                if (source == null || !source.isDirectory()) throw new IOException(tr(R.string.could_not_open_the_folder));
                copyDirectory(source, staging, staging.getCanonicalFile(), new long[] {0});
                if (!isValidGameFolder(staging)) {
                    throw new IOException(tr(R.string.the_folder_must_have_default_xex));
                }
                if (root.exists()) Files.move(root.toPath(), previous.toPath(), StandardCopyOption.REPLACE_EXISTING);
                Files.move(staging.toPath(), root.toPath(), StandardCopyOption.REPLACE_EXISTING);
                deleteRecursively(previous);
                mainHandler.post(() -> showGameFolder(tr(R.string.game_copied)));
            } catch (Exception error) {
                try { deleteRecursively(staging); } catch (IOException ignored) { }
                mainHandler.post(() -> {
                    setImportStatus(tr(R.string.import_failed) + error.getMessage());
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
                    message(tr(R.string.could_not_find_that_iso_s));
                    return;
                }
                if (problem != null) {
                    message(iso.getName() + ": " + problem);
                    return;
                }
                GameOptions.set(this, GameOptions.GAME_ISO, iso.getAbsolutePath());
                showGameFolder(tr(R.string.iso_chosen));
                if (shadersFound != null && shadersFound != shaderLibrary(this).isFile()) rebuildPage(GRAPHICS);
            });
        });
    }

    private void copyDirectory(DocumentFile source, File destination, File safeRoot, long[] copiedBytes)
            throws IOException {
        if (!destination.getCanonicalFile().toPath().startsWith(safeRoot.toPath())) {
            throw new IOException(tr(R.string.the_game_folder_has_an_invalid_2));
        }
        if (!destination.mkdirs() && !destination.isDirectory()) throw new IOException(tr(R.string.could_not_create_2) + destination);
        for (DocumentFile child : source.listFiles()) {
            String name = child.getName();
            if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") ||
                    name.contains("/") || name.contains("\\")) {
                throw new IOException(tr(R.string.the_game_folder_has_an_invalid));
            }
            File target = new File(destination, name);
            if (child.isDirectory()) {
                copyDirectory(child, target, safeRoot, copiedBytes);
            } else if (child.isFile()) {
                try (InputStream input = getContentResolver().openInputStream(child.getUri());
                     FileOutputStream output = new FileOutputStream(target)) {
                    if (input == null) throw new IOException(tr(R.string.could_not_read) + name);
                    byte[] buffer = new byte[1024 * 1024];
                    for (int count; (count = input.read(buffer)) != -1; ) {
                        output.write(buffer, 0, count);
                        copiedBytes[0] += count;
                        if ((copiedBytes[0] & ((64L * 1024 * 1024) - 1)) < count) {
                            long total = copiedBytes[0];
                            mainHandler.post(() -> setImportStatus(tr(R.string.copied) +
                                    (total / (1024 * 1024)) + tr(R.string.mib_of_7500)));
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
        if (!file.delete()) throw new IOException(tr(R.string.could_not_delete) + file.getName());
    }

    // ---- Diagnostic report (saved by the player; never sent anywhere) --------------------------------------

    private static String rootReportLabel() {
        return tr(R.string.diagnostics_with_root_zip);
    }

    // The root manager's prompt first (off the UI thread, it waits for the player), then the usual report flow.
    private void requestRootReport() {
        Menu.setRowEnabled(rootReport, false);
        Menu.titleOf(rootReport).setText(tr(R.string.asking_for_root));
        worker.execute(() -> {
            boolean granted = RootDiagnostics.available();
            mainHandler.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                Menu.setRowEnabled(rootReport, true);
                Menu.titleOf(rootReport).setText(rootReportLabel());
                if (granted) {
                    prepareReport(true);
                } else {
                    message(tr(R.string.no_root_the_phone_is_not_2));
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
                message(tr(R.string.no_root_the_phone_is_not));
            });
        });
    }

    // First what happened, in the player's words: it goes into relatorio.txt, where the report used to end with
    // an unanswered "describe what happened".
    private void prepareReport(boolean root) {
        android.widget.EditText description = new android.widget.EditText(this);
        description.setHint(tr(R.string.what_happened_how_to_reproduce_it));
        description.setMinLines(3);
        description.setMaxLines(8);
        description.setGravity(Gravity.TOP | Gravity.START);
        description.setInputType(android.text.InputType.TYPE_CLASS_TEXT
                | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE | android.text.InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        android.widget.FrameLayout box = new android.widget.FrameLayout(this);
        box.setPadding(dp(20), dp(8), dp(20), 0);
        box.addView(description);
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(tr(R.string.diagnostics))
                .setView(box)
                .setPositiveButton(tr(R.string.create_report),
                        (dialog, which) -> createReport(description.getText().toString(), root))
                .setNegativeButton(tr(R.string.cancel), null)
                .show();
    }

    private void createReport(String description, boolean root) {
        LinearLayout row = root ? rootReport : saveReport;
        String label = root ? rootReportLabel() : reportLabel();
        Menu.setRowEnabled(row, false);
        Menu.titleOf(row).setText(tr(R.string.preparing_diagnostics));
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
                        message(tr(R.string.there_is_no_app_to_save));
                    }
                });
            } catch (Exception error) {
                mainHandler.post(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    Menu.setRowEnabled(row, true);
                    Menu.titleOf(row).setText(label);
                    message(tr(R.string.could_not_prepare_the_report) +
                            error.getMessage());
                });
            }
        });
    }

    private void finishReportSave(Uri destination) {
        File report = pendingReport;
        pendingReport = null;
        if (report == null || !report.isFile()) {
            message(tr(R.string.the_report_is_no_longer_there));
            return;
        }
        worker.execute(() -> {
            try (InputStream input = new FileInputStream(report);
                 OutputStream output = getContentResolver().openOutputStream(destination, "w")) {
                if (output == null) throw new IOException(tr(R.string.could_not_open_the_target_file));
                byte[] buffer = new byte[16384];
                for (int count; (count = input.read(buffer)) != -1; ) output.write(buffer, 0, count);
                mainHandler.post(() -> message(tr(R.string.diagnostics_saved)));
            } catch (Exception error) {
                mainHandler.post(() -> message(tr(R.string.could_not_save_the_zip) + error.getMessage()));
            }
        });
    }

    private void message(String text) {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(tr(R.string.diagnostics)).setMessage(text).setPositiveButton("OK", null).show();
    }

    // ---- Small helpers -------------------------------------------------------------------------------------

    private static String storageNotice() {
        return tr(R.string.to_use_internal_storage_allow_the);
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
        homeVideo.close();
        super.onDestroy();
    }
}
