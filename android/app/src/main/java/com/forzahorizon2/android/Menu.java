package com.forzahorizon2.android;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.Interpolator;
import android.view.animation.PathInterpolator;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.slider.LabelFormatter;
import com.google.android.material.slider.Slider;

/**
 * The launcher's look, after the iPadOS Settings app: a sidebar on the left, and on the right grouped lists (rounded
 * groups of rows split by hairlines, a small caption above and a note below). Every setting is one row: the name, a
 * line saying what it does, and its control at the right (a switch) or below it (a segmented control or a slider).
 *
 * Motion is kept to what tells the player something: a pressed row or button shrinks a little (it heard the touch),
 * a segmented control's pill slides to the new choice, a page or a renderer's options fade in. All of it goes through
 * ViewPropertyAnimator, so the system's animation scale (and "remove animations") applies.
 */
final class Menu {
    static final int ACCENT = 0xFFE6007E;        // the Horizon magenta
    static final int ACCENT_TEXT = 0xFFFF5AAE;   // the magenta lightened for text and icons on the dark background
    static final int ACCENT_SOFT = 0x2EE6007E;   // the sidebar's chosen row
    static final int BACKGROUND = 0xFF09090B;
    static final int SIDEBAR = 0xFF131316;
    static final int GROUP = 0xFF1B1B1F;
    static final int FILL = 0xFF2A2A2F;          // tracks: segmented control, slider, switch off
    static final int SEPARATOR = 0xFF2C2C31;
    static final int PRESSED = 0xFF26262B;
    static final int PRIMARY = 0xFFF5F5F7;
    static final int SECONDARY = 0xFF8E8E96;
    static final int TERTIARY = 0xFF5C5C63;
    static final int GREEN = 0xFF30D158;
    static final int RED = 0xFFFF453A;
    static final int AMBER = 0xFFFFB340;

    /** Strong ease-out: starts at once, settles softly. For everything that enters or moves in the menu. */
    static final Interpolator EASE_OUT = new PathInterpolator(0.23f, 1f, 0.32f, 1f);

    private static final Object DIVIDER = "divider";

    interface OnFlag {
        void set(boolean value);
    }

    interface OnChoice {
        void chosen(String value);
    }

    interface OnIndex {
        void chosen(int index);
    }

    private final Context context;

    Menu(Context context) {
        this.context = context;
    }

    // ---- Basics --------------------------------------------------------------------------------------------

    static int dp(Context context, float value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }

    int dp(float value) {
        return dp(context, value);
    }

    static GradientDrawable rounded(int color, float radius) {
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(color);
        shape.setCornerRadius(radius);
        return shape;
    }

    /** Shrinks the view to 97 % while it is held: the touch was heard before anything else happens. */
    @SuppressLint("ClickableViewAccessibility")
    static void pressable(View view) {
        view.setOnTouchListener((v, event) -> {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    if (v.isEnabled()) v.animate().scaleX(0.97f).scaleY(0.97f).setDuration(100).setInterpolator(EASE_OUT).start();
                    break;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    v.animate().scaleX(1f).scaleY(1f).setDuration(160).setInterpolator(EASE_OUT).start();
                    break;
            }
            return false;  // the click still goes to the view
        });
    }

    /** Fades a view in from slightly below: a page, or the options of the chosen renderer. */
    static void enter(View view) {
        view.animate().cancel();
        view.setAlpha(0f);
        view.setTranslationY(dp(view.getContext(), 10));
        view.animate().alpha(1f).translationY(0f).setDuration(220).setInterpolator(EASE_OUT).start();
    }

    LinearLayout column() {
        LinearLayout column = new LinearLayout(context);
        column.setOrientation(LinearLayout.VERTICAL);
        return column;
    }

    TextView text(String value, float sp, int color, boolean bold) {
        TextView t = new TextView(context);
        t.setText(value);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setTypeface(Typeface.create("sans-serif" + (bold ? "-medium" : ""), bold ? Typeface.BOLD : Typeface.NORMAL));
        return t;
    }

    private static LinearLayout.LayoutParams matchWidth() {
        return new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    // ---- Page structure ------------------------------------------------------------------------------------

    /** A page's large title and the line under it. */
    View pageTitle(String title, String subtitle) {
        LinearLayout box = column();
        box.setPadding(dp(4), 0, dp(4), dp(6));
        TextView t = text(title, 30, PRIMARY, true);
        t.setLetterSpacing(-0.01f);
        box.addView(t);
        if (subtitle != null) {
            TextView s = text(subtitle, 14, SECONDARY, false);
            s.setPadding(0, dp(2), 0, 0);
            box.addView(s);
        }
        return box;
    }

    /** The small caption above a group. */
    TextView header(String value) {
        TextView t = text(value.toUpperCase(java.util.Locale.ROOT), 12, SECONDARY, false);
        t.setLetterSpacing(0.06f);
        t.setPadding(dp(16), dp(22), dp(16), dp(7));
        return t;
    }

    /** The note below a group. */
    TextView footer(String value) {
        TextView t = text(value, 12.5f, SECONDARY, false);
        t.setLineSpacing(0, 1.15f);
        t.setPadding(dp(16), dp(7), dp(16), 0);
        return t;
    }

    /** A rounded group of rows with hairlines between them. */
    LinearLayout group(View... rows) {
        LinearLayout group = column();
        group.setBackground(rounded(GROUP, dp(14)));
        group.setClipToOutline(true);
        for (View row : rows) if (row != null) group.addView(row, matchWidth());
        refreshDividers(group);
        return group;
    }

    /** A hairline above every visible row but the first. Called again when a row is shown or hidden. */
    static void refreshDividers(ViewGroup group) {
        boolean first = true;
        for (int i = 0; i < group.getChildCount(); i++) {
            View row = group.getChildAt(i);
            View divider = row.findViewWithTag(DIVIDER);
            if (row.getVisibility() == View.GONE) continue;
            if (divider != null) divider.setVisibility(first ? View.GONE : View.VISIBLE);
            first = false;
        }
    }

    /** A row's frame: the hairline (shown by refreshDividers) and the content under it. */
    private LinearLayout frame(View content) {
        LinearLayout frame = column();
        View divider = new View(context);
        divider.setTag(DIVIDER);
        divider.setBackgroundColor(SEPARATOR);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1);
        lp.leftMargin = dp(16);
        frame.addView(divider, lp);
        frame.addView(content, matchWidth());
        return frame;
    }

    /** The name and description of a setting. */
    private LinearLayout labels(String title, String description) {
        LinearLayout box = column();
        box.addView(text(title, 16, PRIMARY, false));
        if (description != null && !description.isEmpty()) {
            TextView d = text(description, 13, SECONDARY, false);
            d.setLineSpacing(0, 1.12f);
            d.setPadding(0, dp(3), 0, 0);
            box.addView(d);
        }
        return box;
    }

    /** A row with the labels on the left and an optional view on the right. */
    LinearLayout row(String title, String description, View trailing) {
        LinearLayout content = new LinearLayout(context);
        content.setOrientation(LinearLayout.HORIZONTAL);
        content.setGravity(Gravity.CENTER_VERTICAL);
        content.setMinimumHeight(dp(52));
        content.setPadding(dp(16), dp(11), dp(16), dp(11));
        content.addView(labels(title, description), new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        if (trailing != null) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.leftMargin = dp(14);
            content.addView(trailing, lp);
        }
        return frame(content);
    }

    /** A row of the caller's own layout, with the group's hairline above it. */
    LinearLayout plainRow(View content) {
        return frame(content);
    }

    /** A row with a control below the labels, the whole width of the group. */
    LinearLayout stackedRow(String title, String description, View control) {
        LinearLayout content = column();
        content.setPadding(dp(16), dp(12), dp(16), dp(14));
        content.addView(labels(title, description));
        LinearLayout.LayoutParams lp = matchWidth();
        lp.topMargin = dp(10);
        content.addView(control, lp);
        return frame(content);
    }

    // ---- Settings ------------------------------------------------------------------------------------------

    MaterialSwitch toggle(boolean value, OnFlag save) {
        MaterialSwitch s = new MaterialSwitch(context);
        s.setThumbTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}},
                new int[] {Color.WHITE, 0xFFB8B8BF}));
        s.setTrackTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}},
                new int[] {ACCENT, FILL}));
        s.setTrackDecorationTintList(ColorStateList.valueOf(Color.TRANSPARENT));
        s.setChecked(value);
        s.setOnCheckedChangeListener((v, on) -> save.set(on));
        return s;
    }

    /** A setting that is on or off. Tapping anywhere on the row flips it. */
    LinearLayout toggleRow(String title, String description, boolean value, OnFlag save) {
        MaterialSwitch s = toggle(value, save);
        LinearLayout row = row(title, description, s);
        View content = row.getChildAt(1);
        content.setBackground(pressedBackground());
        content.setOnClickListener(v -> s.toggle());
        return row;
    }

    /** One of a few choices, stored under a GameOptions key. */
    LinearLayout choiceRow(String key, String title, String description, String[] labels, String[] values,
                           OnChoice after) {
        SegmentedControl control = new SegmentedControl(context, labels);
        control.choose(java.util.Arrays.asList(values).indexOf(GameOptions.get(context, key)), false);
        control.setOnChoice(index -> {
            GameOptions.set(context, key, values[index]);
            if (after != null) after.chosen(values[index]);
        });
        return stackedRow(title, description, control);
    }

    LinearLayout choiceRow(String key, String title, String description, String[] labels, String[] values) {
        return choiceRow(key, title, description, labels, values, null);
    }

    /**
     * One of a list of choices, as a drop-down: the chosen one and a ▾ at the row's right, and tapping anywhere on
     * the row opens the list under it, the chosen one in magenta.
     */
    LinearLayout dropdownRow(String title, String description, String[] labels, int chosen, OnIndex choose) {
        int[] current = {Math.max(0, chosen)};
        TextView value = text(labels[current[0]] + "  ▾", 15, ACCENT_TEXT, false);
        LinearLayout row = row(title, description, value);
        View content = row.getChildAt(1);
        content.setBackground(pressedBackground());
        content.setOnClickListener(v -> {
            android.widget.ListPopupWindow list = new android.widget.ListPopupWindow(context);
            list.setAdapter(new android.widget.ArrayAdapter<String>(context, 0, labels) {
                @Override
                public View getView(int i, View reuse, ViewGroup parent) {
                    TextView item = text(labels[i], 15, i == current[0] ? ACCENT_TEXT : PRIMARY, i == current[0]);
                    item.setPadding(dp(16), dp(12), dp(16), dp(12));
                    return item;
                }
            });
            list.setBackgroundDrawable(rounded(PRESSED, dp(12)));
            list.setAnchorView(value);
            list.setDropDownGravity(Gravity.END);
            list.setContentWidth(dp(200));
            list.setModal(true);
            list.setOnItemClickListener((parent, item, i, id) -> {
                list.dismiss();
                if (i == current[0]) return;
                current[0] = i;
                value.setText(labels[i] + "  ▾");
                choose.chosen(i);
            });
            list.show();
        });
        return row;
    }

    /** A slider in the menu's colors, with no floating label (the row shows the value). */
    Slider slider(float from, float to, float step, float value) {
        Slider s = new Slider(context);
        s.setValueFrom(from);
        s.setValueTo(to);
        s.setStepSize(step);
        s.setValue(Math.max(from, Math.min(to, value)));
        s.setLabelBehavior(LabelFormatter.LABEL_GONE);
        s.setTickVisible(false);
        s.setTrackActiveTintList(ColorStateList.valueOf(ACCENT));
        s.setTrackInactiveTintList(ColorStateList.valueOf(FILL));
        s.setThumbTintList(ColorStateList.valueOf(Color.WHITE));
        s.setHaloTintList(ColorStateList.valueOf(0x33E6007E));
        s.setThumbElevation(dp(2));
        s.setTrackHeight(dp(4));
        return s;
    }

    /** A row that does something when tapped: an action in magenta (red when destructive), with a chevron. */
    LinearLayout actionRow(String title, String description, boolean destructive, View.OnClickListener action) {
        LinearLayout row = row(title, description, chevron());
        View content = row.getChildAt(1);
        TextView name = titleOf(row);
        name.setTextColor(new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}},
                new int[] {TERTIARY, destructive ? RED : ACCENT_TEXT}));
        content.setBackground(pressedBackground());
        content.setOnClickListener(action);
        return row;
    }

    /** The title TextView of a row made here, to change its text later. */
    static TextView titleOf(LinearLayout row) {
        ViewGroup content = (ViewGroup) row.getChildAt(1);
        ViewGroup labels = (ViewGroup) content.getChildAt(0);
        return (TextView) labels.getChildAt(0);
    }

    /** The description TextView of a row made here with one (under the title). */
    static TextView descriptionOf(LinearLayout row) {
        ViewGroup content = (ViewGroup) row.getChildAt(1);
        ViewGroup labels = (ViewGroup) content.getChildAt(0);
        return (TextView) labels.getChildAt(1);
    }

    /** The switch of a row made with toggleRow, to turn it back without a tap. */
    static MaterialSwitch switchOf(LinearLayout row) {
        ViewGroup content = (ViewGroup) row.getChildAt(1);
        return (MaterialSwitch) content.getChildAt(1);
    }

    /** Enables or disables a row made here: it stops taking taps and its texts dim. */
    static void setRowEnabled(LinearLayout row, boolean enabled) {
        View content = row.getChildAt(1);
        content.setEnabled(enabled);
        content.setAlpha(enabled ? 1f : 0.45f);
        if (content instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) content;
            for (int i = 0; i < group.getChildCount(); i++) setEnabledDeep(group.getChildAt(i), enabled);
        }
    }

    static void setEnabledDeep(View view, boolean enabled) {
        view.setEnabled(enabled);
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) setEnabledDeep(group.getChildAt(i), enabled);
        }
    }

    /** One of a list where the chosen one has a check mark (iOS's checklist). */
    LinearLayout checkRow(String title, String description, boolean checked, View.OnClickListener choose) {
        TextView mark = text(checked ? "✓" : "", 18, ACCENT_TEXT, true);
        LinearLayout row = row(title, description, mark);
        View content = row.getChildAt(1);
        content.setBackground(pressedBackground());
        content.setOnClickListener(choose);
        return row;
    }

    /** A line with a state mark at the right: a check in green, a cross in red. */
    LinearLayout statusRow(String title, String description, boolean ok, String state) {
        return statusRow(title, description, ok ? GREEN : RED, state);
    }

    /** As above, in any color (amber for "in progress"). */
    LinearLayout statusRow(String title, String description, int color, String state) {
        LinearLayout mark = new LinearLayout(context);
        mark.setOrientation(LinearLayout.HORIZONTAL);
        mark.setGravity(Gravity.CENTER_VERTICAL);
        TextView label = text(state, 13, color, true);
        mark.addView(dot(color));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.leftMargin = dp(6);
        if (state != null) mark.addView(label, lp);
        return row(title, description, mark);
    }

    View dot(int color) {
        View dot = new View(context);
        GradientDrawable shape = new GradientDrawable();
        shape.setShape(GradientDrawable.OVAL);
        shape.setColor(color);
        dot.setBackground(shape);
        dot.setLayoutParams(new LinearLayout.LayoutParams(dp(8), dp(8)));
        return dot;
    }

    /** A small rounded tag, as "Experimental" next to a name. */
    TextView badge(String value, int color) {
        TextView t = text(value.toUpperCase(java.util.Locale.ROOT), 10.5f, color, true);
        t.setLetterSpacing(0.05f);
        t.setPadding(dp(8), dp(3), dp(8), dp(3));
        t.setBackground(rounded((color & 0x00FFFFFF) | 0x26000000, dp(6)));
        t.setSingleLine(true);
        t.setEllipsize(TextUtils.TruncateAt.END);
        return t;
    }

    private TextView chevron() {
        return text("›", 22, TERTIARY, false);
    }

    private android.graphics.drawable.Drawable pressedBackground() {
        android.graphics.drawable.StateListDrawable states = new android.graphics.drawable.StateListDrawable();
        states.addState(new int[] {android.R.attr.state_pressed}, new android.graphics.drawable.ColorDrawable(PRESSED));
        states.addState(new int[] {}, new android.graphics.drawable.ColorDrawable(Color.TRANSPARENT));
        states.setExitFadeDuration(180);
        return states;
    }
}
