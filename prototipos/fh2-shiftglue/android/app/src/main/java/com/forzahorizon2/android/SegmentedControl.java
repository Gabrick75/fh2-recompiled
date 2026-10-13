package com.forzahorizon2.android;

import android.content.Context;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The menu's choice control, as iOS's segmented control: the options side by side on a dark track and a magenta pill
 * under the chosen one, which slides to the new one when another is tapped. The slide is the feedback that the
 * choice changed, so it is short (220 ms) and ease-out; a choice made from code jumps without animating.
 */
final class SegmentedControl extends FrameLayout {
    interface OnChoice {
        void chosen(int index);
    }

    private final View pill;
    private final LinearLayout labels;
    private int chosen = -1;
    private OnChoice listener;

    SegmentedControl(Context context, String[] options) {
        super(context);
        int inset = Menu.dp(context, 3);
        setPadding(inset, inset, inset, inset);
        setBackground(Menu.rounded(Menu.FILL, Menu.dp(context, 11)));

        pill = new View(context);
        GradientDrawable pillShape = Menu.rounded(Menu.ACCENT, Menu.dp(context, 8));
        pill.setBackground(pillShape);
        pill.setElevation(Menu.dp(context, 2));
        addView(pill, new LayoutParams(0, LayoutParams.MATCH_PARENT));

        labels = new LinearLayout(context);
        labels.setOrientation(LinearLayout.HORIZONTAL);
        labels.setElevation(Menu.dp(context, 3));  // above the pill
        for (int i = 0; i < options.length; i++) {
            TextView label = new TextView(context);
            label.setText(options[i]);
            label.setGravity(Gravity.CENTER);
            label.setSingleLine(true);
            label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
            label.setTextColor(Menu.SECONDARY);
            label.setPadding(Menu.dp(context, 6), 0, Menu.dp(context, 6), 0);
            int index = i;
            label.setOnClickListener(v -> {
                if (index == chosen) return;
                choose(index, true);
                if (listener != null) listener.chosen(index);
            });
            Menu.pressable(label);
            labels.addView(label, new LinearLayout.LayoutParams(0, Menu.dp(context, 34), 1f));
        }
        addView(labels, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
    }

    void setOnChoice(OnChoice listener) {
        this.listener = listener;
    }

    void choose(int index, boolean animate) {
        for (int i = 0; i < labels.getChildCount(); i++) {
            TextView label = (TextView) labels.getChildAt(i);
            label.setTextColor(i == index ? Menu.PRIMARY : Menu.SECONDARY);
            label.setTypeface(Typeface.create("sans-serif-medium", i == index ? Typeface.BOLD : Typeface.NORMAL));
        }
        pill.setVisibility(index < 0 ? INVISIBLE : VISIBLE);
        chosen = index;
        if (index < 0) return;
        if (getWidth() == 0) {
            post(() -> placePill(false));  // before the first layout: placed once the width is known
        } else {
            placePill(animate);
        }
    }

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        super.onLayout(changed, left, top, right, bottom);
        if (changed && chosen >= 0) placePill(false);
    }

    private void placePill(boolean animate) {
        if (chosen < 0 || labels.getChildCount() == 0) return;
        View target = labels.getChildAt(chosen);
        int width = target.getWidth();
        if (width == 0) return;
        if (pill.getLayoutParams().width != width) {
            pill.getLayoutParams().width = width;
            pill.requestLayout();
        }
        pill.animate().cancel();
        if (animate) {
            pill.animate().translationX(target.getLeft()).setDuration(220).setInterpolator(Menu.EASE_OUT).start();
        } else {
            pill.setTranslationX(target.getLeft());
        }
    }
}
