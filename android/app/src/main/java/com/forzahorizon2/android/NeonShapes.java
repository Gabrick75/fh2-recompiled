package com.forzahorizon2.android;

import android.graphics.Canvas;
import android.graphics.CornerPathEffect;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.Shader;
import android.graphics.drawable.Drawable;

/** The launcher's racing-game shapes that a GradientDrawable cannot draw. */
final class NeonShapes {
    private NeonShapes() {
    }

    /**
     * The sidebar's panel: translucent black whose right edge leans in towards the foot (the top is the widest),
     * with a faint magenta line along that edge. The same on every page.
     */
    static final class SlantedPanel extends Drawable {
        private final float slant;
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint edge = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path shape = new Path();

        SlantedPanel(float slant, float density) {
            this.slant = slant;
            edge.setStyle(Paint.Style.STROKE);
            edge.setStrokeWidth(1.5f * density);
        }

        @Override
        protected void onBoundsChange(Rect b) {
            shape.reset();
            shape.moveTo(b.left, b.top);
            shape.lineTo(b.right, b.top);
            shape.lineTo(b.right - slant, b.bottom);
            shape.lineTo(b.left, b.bottom);
            shape.close();
            fill.setShader(new LinearGradient(0, b.top, 0, b.bottom, 0xEB070709, 0xE00C0C0F, Shader.TileMode.CLAMP));
            // Faint at the top, bright magenta at the foot, as the reference's.
            edge.setShader(new LinearGradient(0, b.top, 0, b.bottom, 0x33FF2E9A, 0xF2FF2E9A, Shader.TileMode.CLAMP));
        }

        /** The panel's outline, to clip what lies on it (a chosen row's wash ends at the slanted edge). */
        Path shape() {
            return shape;
        }

        @Override
        public void draw(Canvas canvas) {
            Rect b = getBounds();
            canvas.drawPath(shape, fill);
            float half = edge.getStrokeWidth() / 2;  // the whole line inside the bounds
            canvas.drawLine(b.right - half, b.top, b.right - slant - half, b.bottom, edge);
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

    /**
     * A see-through card leaning as the Play plate's left edge does (both sides), whose pink border shows at the
     * bottom left and fades out before it closes the round.
     */
    static final class FadedBorderCard extends Drawable {
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint border = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path shape = new Path();

        FadedBorderCard(int color, float radius, float density) {
            CornerPathEffect corners = new CornerPathEffect(radius);
            fill.setColor(color);
            fill.setPathEffect(corners);
            border.setStyle(Paint.Style.STROKE);
            border.setStrokeWidth(1.2f * density);
            border.setPathEffect(corners);
        }

        @Override
        protected void onBoundsChange(Rect b) {
            float half = border.getStrokeWidth() / 2;
            float l = b.left + half, t = b.top + half, r = b.right - half, btm = b.bottom - half;
            float lean = (btm - t) * 0.22f;  // PlayBackground.lean's slope
            shape.reset();
            shape.moveTo(l + lean, t);
            shape.lineTo(r, t);
            shape.lineTo(r - lean, btm);
            shape.lineTo(l, btm);
            shape.close();
            // Pink at the bottom left, fading out towards the top right.
            border.setShader(new LinearGradient(b.left, b.bottom, b.left + b.width() * 0.7f, b.top,
                    0xA6FF2E9A, 0x00FF2E9A, Shader.TileMode.CLAMP));
        }

        @Override
        public void draw(Canvas canvas) {
            canvas.drawPath(shape, fill);
            canvas.drawPath(shape, border);
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
}
