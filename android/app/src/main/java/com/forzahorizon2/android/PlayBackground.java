package com.forzahorizon2.android;

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
 * The Play buttons' background, after a racing game's: a slanted plate in a magenta gradient with three diagonal
 * stripes at its right end. It follows the button's state: pressed, a deeper magenta and a stronger glow (the button
 * itself shrinks, Menu.pressable); focused by a keyboard or a controller, a white ring around it; disabled, washed
 * out. With the game missing it is a plain dark plate, as the old grey button was.
 */
final class PlayBackground extends Drawable {
    private final float density;
    private final boolean ready;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stripe = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint glow = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path plate = new Path();
    private final Path stripes = new Path();
    private boolean pressed, focused, enabled = true;

    PlayBackground(float density, boolean ready) {
        this.density = density;
        this.ready = ready;
        CornerPathEffect corners = new CornerPathEffect(8 * density);
        fill.setPathEffect(corners);
        glow.setPathEffect(corners);
        glow.setStyle(Paint.Style.STROKE);
        ring.setPathEffect(corners);
        ring.setStyle(Paint.Style.STROKE);
        ring.setStrokeWidth(2 * density);
        ring.setColor(0xFFFFFFFF);
        stripe.setColor(Menu.ACCENT);
    }

    /** How far the plate leans: the top edge is this much to the right of the bottom one. */
    private float lean(Rect b) {
        return b.height() * 0.22f;
    }

    @Override
    protected void onBoundsChange(Rect b) {
        float inset = 3 * density;  // room for the focus ring and the glow
        float l = b.left + inset, t = b.top + inset, r = b.right - inset, btm = b.bottom - inset;
        float lean = lean(b);
        float stripesWidth = ready ? b.height() * 0.62f : 0;
        float plateRight = r - stripesWidth;
        plate.reset();
        plate.moveTo(l + lean, t);
        plate.lineTo(plateRight, t);
        plate.lineTo(plateRight - lean, btm);
        plate.lineTo(l, btm);
        plate.close();
        stripes.reset();
        if (ready) {
            float w = 4 * density, gap = 5 * density, x = plateRight + gap;
            for (int i = 0; i < 2; i++, x += w + gap) {
                stripes.moveTo(x, t);
                stripes.lineTo(x + w, t);
                stripes.lineTo(x + w - lean, btm);
                stripes.lineTo(x - lean, btm);
                stripes.close();
            }
        }
        updatePaints();
    }

    private void updatePaints() {
        Rect b = getBounds();
        int top, bottom;
        if (!ready) {
            top = 0xE62A2A2F;
            bottom = 0xE61F1F23;
        } else if (pressed) {
            top = 0xFFC8006E;
            bottom = 0xFF8A0050;
        } else {
            top = 0xFFFF2E9A;
            bottom = 0xFFC4006B;
        }
        fill.setShader(new LinearGradient(b.left, b.top, b.right, b.bottom, top, bottom, Shader.TileMode.CLAMP));
        glow.setStrokeWidth((pressed ? 6 : 4) * density);
        glow.setColor(ready ? (pressed ? 0x66FF2E9A : 0x38FF2E9A) : 0);
        ColorMatrix washed = new ColorMatrix();
        washed.setSaturation(0.25f);
        ColorMatrixColorFilter filter = enabled ? null : new ColorMatrixColorFilter(washed);
        fill.setColorFilter(filter);
        stripe.setColorFilter(filter);
        fill.setAlpha(enabled ? 255 : 150);
        stripe.setAlpha(enabled ? 255 : 110);
        glow.setAlpha(enabled ? glow.getAlpha() : 0);
    }

    @Override
    public void draw(Canvas canvas) {
        canvas.drawPath(plate, glow);
        canvas.drawPath(plate, fill);
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
        int inset = Math.round(4 * density);
        outline.setRoundRect(b.left + inset, b.top + inset, b.right - inset, b.bottom - inset, 8 * density);
        outline.setAlpha(ready && enabled ? 0.9f : 0f);
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
