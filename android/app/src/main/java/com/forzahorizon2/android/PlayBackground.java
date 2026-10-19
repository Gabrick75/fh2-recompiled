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
 * The Play buttons' background, after a racing game's: a slanted plate in a magenta gradient with diagonal stripes
 * at its right end. It follows the button's state: pressed, a deeper magenta and a stronger glow (the button itself
 * shrinks, Menu.pressable); focused by a keyboard or a controller, a white ring around it; disabled, washed out.
 * With the game missing it is a plain dark plate, as the old grey button was.
 *
 * The large one (the home page's) also has a soft blurred glow around it, a light sheen on its top half and a thin
 * bright rim; it keeps a margin inside its bounds for the glow, so its view must use a software layer (the blur).
 */
final class PlayBackground extends Drawable {
    private final float density;
    private final boolean ready;
    private final boolean large;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint sheen = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint rim = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stripe = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint glow = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path plate = new Path();
    private final Path stripes = new Path();
    private boolean pressed, focused, enabled = true;

    PlayBackground(float density, boolean ready, boolean large) {
        this.density = density;
        this.ready = ready;
        this.large = large;
        CornerPathEffect corners = new CornerPathEffect((large ? 10 : 8) * density);
        fill.setPathEffect(corners);
        sheen.setPathEffect(corners);
        rim.setPathEffect(corners);
        rim.setStyle(Paint.Style.STROKE);
        rim.setStrokeWidth(1.2f * density);
        glow.setPathEffect(corners);
        if (large) glow.setMaskFilter(new BlurMaskFilter(14 * density, BlurMaskFilter.Blur.NORMAL));
        else glow.setStyle(Paint.Style.STROKE);
        ring.setPathEffect(corners);
        ring.setStyle(Paint.Style.STROKE);
        ring.setStrokeWidth(2 * density);
        ring.setColor(0xFFFFFFFF);
        stripe.setColor(0xFFFF2E9A);
    }

    /** The room kept around the plate: for the focus ring, and on the large one for the glow. */
    float inset() {
        return (large ? 14 : 3) * density;
    }

    /** How far the plate leans: the top edge is this much to the right of the bottom one. */
    float lean(int height) {
        return (height - 2 * inset()) * 0.22f;
    }

    /** The width the stripes take at the right end, past the plate. */
    float stripesWidth(int height) {
        return ready ? (height - 2 * inset()) * (large ? 0.52f : 0.62f) : 0;
    }

    @Override
    protected void onBoundsChange(Rect b) {
        float inset = inset();
        float l = b.left + inset, t = b.top + inset, r = b.right - inset, btm = b.bottom - inset;
        float lean = lean(b.height());
        float plateRight = r - stripesWidth(b.height());
        plate.reset();
        plate.moveTo(l + lean, t);
        plate.lineTo(plateRight, t);
        plate.lineTo(plateRight - lean, btm);
        plate.lineTo(l, btm);
        plate.close();
        stripes.reset();
        if (ready) {
            float w = (large ? 5 : 4) * density, gap = (large ? 7 : 5) * density, x = plateRight + gap;
            for (int i = 0; i < 2; i++, x += w + gap) {
                stripes.moveTo(x, t);
                stripes.lineTo(x + w, t);
                stripes.lineTo(x + w - lean, btm);
                stripes.lineTo(x - lean, btm);
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
        int left, right;
        if (!ready) {
            left = 0xE62A2A2F;
            right = 0xE61F1F23;
        } else if (pressed) {
            left = 0xFFE0168A;
            right = 0xFF8A0050;
        } else {
            left = 0xFFFF3DA8;
            right = 0xFFC2006C;
        }
        fill.setShader(new LinearGradient(b.left, b.top, b.right, b.bottom, left, right, Shader.TileMode.CLAMP));
        if (large) {
            glow.setColor(ready ? (pressed ? 0xCCFF2E9A : 0x80FF2E9A) : 0);
        } else {
            glow.setStrokeWidth((pressed ? 6 : 4) * density);
            glow.setColor(ready ? (pressed ? 0x66FF2E9A : 0x38FF2E9A) : 0);
        }
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
        if (large) canvas.drawPath(stripes, glow);
        canvas.drawPath(plate, fill);
        if (large && ready && enabled) {
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
        Rect b = getBounds();
        int inset = Math.round(inset() + density);
        outline.setRoundRect(b.left + inset, b.top + inset, b.right - inset, b.bottom - inset, 8 * density);
        // The large one glows by itself: no system shadow under it.
        outline.setAlpha(ready && enabled && !large ? 0.9f : 0f);
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
