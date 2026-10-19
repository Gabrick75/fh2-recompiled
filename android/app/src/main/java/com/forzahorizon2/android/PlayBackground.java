package com.forzahorizon2.android;

import android.graphics.BlurMaskFilter;
import android.graphics.Canvas;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.CornerPathEffect;
import android.graphics.LinearGradient;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.Shader;
import android.graphics.drawable.Drawable;

/**
 * The Play buttons' background, after a racing game's: a slanted plate in a magenta gradient with a sheen on its top
 * half, a bright rim and a soft blurred glow, and two broad bands at its right end parallel to its steep right edge.
 * It follows the button's state: pressed, a deeper magenta (plate and bands) and a stronger glow (the button itself
 * shrinks, Menu.pressable); focused by a keyboard or a controller, a white ring around it; disabled, washed out.
 * With the game missing it is a plain dark plate, as the old grey button was.
 *
 * The home page's and the sidebar's are the same, only of different sizes. It keeps a margin inside its bounds for
 * the glow, and its view must use a software layer (the blur).
 */
final class PlayBackground extends Drawable {
    private final float density;
    private final boolean ready;
    private final boolean bands;
    private final float glowRoom;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint sheen = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint rim = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stripe = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint glow = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path plate = new Path();
    private final Path stripes = new Path();
    private boolean pressed, focused, enabled = true;

    /** glowRoomDp: the margin kept around the plate for the glow (and the focus ring). */
    PlayBackground(float density, boolean ready, float glowRoomDp) {
        this(density, ready, glowRoomDp, true);
    }

    /**
     * ready: the magenta plate (false: the dark one, as a secondary button's); bands: the two bands at its right end
     * (the Play buttons'; the name dialog's buttons go without).
     */
    PlayBackground(float density, boolean ready, float glowRoomDp, boolean bands) {
        this.density = density;
        this.ready = ready;
        this.bands = bands;
        this.glowRoom = glowRoomDp * density;
        CornerPathEffect corners = new CornerPathEffect(glowRoom * 0.7f);
        fill.setPathEffect(corners);
        sheen.setPathEffect(corners);
        rim.setPathEffect(corners);
        rim.setStyle(Paint.Style.STROKE);
        rim.setStrokeWidth(1.2f * density);
        glow.setPathEffect(corners);
        glow.setMaskFilter(new BlurMaskFilter(glowRoom, BlurMaskFilter.Blur.NORMAL));
        stripe.setPathEffect(new CornerPathEffect(glowRoom * 0.3f));
        ring.setPathEffect(corners);
        ring.setStyle(Paint.Style.STROKE);
        ring.setStrokeWidth(2 * density);
        ring.setColor(0xFFFFFFFF);
    }

    /** The room kept around the plate. */
    float inset() {
        return glowRoom;
    }

    /** How far the plate's left edge leans: its top is this much to the right of its foot. */
    float lean(int height) {
        return (height - 2 * inset()) * 0.22f;
    }

    /** The same for the right edge and the bands: much steeper (the reference's). */
    float rightLean(int height) {
        return (height - 2 * inset()) * 0.45f;
    }

    /** The width the bands take at the right end, past the plate's top. */
    float stripesWidth(int height) {
        return ready && bands ? (height - 2 * inset()) * 0.6f : 0;
    }

    @Override
    protected void onBoundsChange(Rect b) {
        float inset = inset();
        float l = b.left + inset, t = b.top + inset, r = b.right - inset, btm = b.bottom - inset;
        float lean = lean(b.height()), rightLean = rightLean(b.height());
        float plateRight = r - stripesWidth(b.height());
        plate.reset();
        plate.moveTo(l + lean, t);
        plate.lineTo(plateRight, t);
        plate.lineTo(plateRight - rightLean, btm);
        plate.lineTo(l, btm);
        plate.close();
        stripes.reset();
        if (ready && bands) {
            float h = btm - t, w = h * 0.2f, gap = h * 0.1f, x = plateRight + gap;
            for (int i = 0; i < 2; i++, x += w + gap) {
                stripes.moveTo(x, t);
                stripes.lineTo(x + w, t);
                stripes.lineTo(x + w - rightLean, btm);
                stripes.lineTo(x - rightLean, btm);
                stripes.close();
            }
        }
        sheen.setShader(new LinearGradient(0, t, 0, t + (btm - t) * 0.55f, 0x40FFFFFF, 0x00FFFFFF,
                Shader.TileMode.CLAMP));
        rim.setShader(new LinearGradient(0, t, 0, btm, 0xB3FFB3DB, 0x1AFFB3DB, Shader.TileMode.CLAMP));
        updatePaints();
    }

    private void updatePaints() {
        Rect b = getBounds();
        int left, right, stripeTop, stripeBottom;
        if (!ready) {
            // The dark plate darkens too when pressed.
            left = pressed ? 0xF0161619 : 0xE62A2A2F;
            right = pressed ? 0xF00E0E10 : 0xE61F1F23;
            stripeTop = stripeBottom = 0;
        } else if (pressed) {
            left = 0xFFE0168A;
            right = 0xFF8A0050;
            stripeTop = 0xFFD0157F;
            stripeBottom = 0xFF7A0046;
        } else {
            left = 0xFFFF3DA8;
            right = 0xFFC2006C;
            stripeTop = 0xFFFF3DA8;
            stripeBottom = 0xFFB8005F;
        }
        fill.setShader(new LinearGradient(b.left, b.top, b.right, b.bottom, left, right, Shader.TileMode.CLAMP));
        stripe.setShader(new LinearGradient(0, b.top, 0, b.bottom, stripeTop, stripeBottom, Shader.TileMode.CLAMP));
        glow.setColor(ready ? (pressed ? 0xCCFF2E9A : 0x80FF2E9A) : 0);
        ColorMatrix washed = new ColorMatrix();
        washed.setSaturation(0.25f);
        ColorMatrixColorFilter filter = enabled ? null : new ColorMatrixColorFilter(washed);
        fill.setColorFilter(filter);
        stripe.setColorFilter(filter);
        fill.setAlpha(enabled ? 255 : 150);
        stripe.setAlpha(enabled ? 255 : 110);
        if (!enabled) glow.setAlpha(0);
    }

    @Override
    public void draw(Canvas canvas) {
        canvas.drawPath(plate, glow);
        canvas.drawPath(stripes, glow);
        canvas.drawPath(plate, fill);
        if (ready && enabled) {
            canvas.drawPath(plate, sheen);
            canvas.drawPath(plate, rim);
        }
        canvas.drawPath(stripes, stripe);
        if (focused) {
            canvas.save();
            float s = 1 + 2.5f * density / Math.max(1, getBounds().height());
            canvas.scale(s, s, getBounds().exactCenterX(), getBounds().exactCenterY());
            canvas.drawPath(plate, ring);
            canvas.restore();
        }
    }

    @Override
    public boolean isStateful() {
        return true;
    }

    @Override
    protected boolean onStateChange(int[] state) {
        boolean p = false, f = false, e = false;
        for (int s : state) {
            if (s == android.R.attr.state_pressed) p = true;
            else if (s == android.R.attr.state_focused) f = true;
            else if (s == android.R.attr.state_enabled) e = true;
        }
        if (p == pressed && f == focused && e == enabled) return false;
        pressed = p;
        focused = f;
        enabled = e;
        updatePaints();
        invalidateSelf();
        return true;
    }

    @Override
    public void getOutline(Outline outline) {
        // It glows by itself: no system shadow under it.
        outline.setRect(getBounds());
        outline.setAlpha(0f);
    }

    @Override
    public void setAlpha(int alpha) {
    }

    @Override
    public void setColorFilter(android.graphics.ColorFilter colorFilter) {
    }

    @Override
    public int getOpacity() {
        return PixelFormat.TRANSLUCENT;
    }
}
