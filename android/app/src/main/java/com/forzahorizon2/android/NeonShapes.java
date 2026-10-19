package com.forzahorizon2.android;

import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
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
            edge.setStrokeWidth(1.2f * density);
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
            edge.setShader(new LinearGradient(0, b.top, 0, b.bottom, 0x8CE6007E, 0x26E6007E, Shader.TileMode.CLAMP));
        }

        @Override
        public void draw(Canvas canvas) {
            Rect b = getBounds();
            canvas.drawPath(shape, fill);
            canvas.drawLine(b.right, b.top, b.right - slant, b.bottom, edge);
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

    /** A see-through card whose pink border shows at the top right and fades out before it closes the round. */
    static final class FadedBorderCard extends Drawable {
        private final float radius;
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint border = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF box = new RectF();

        FadedBorderCard(int color, float radius, float density) {
            this.radius = radius;
            fill.setColor(color);
            border.setStyle(Paint.Style.STROKE);
            border.setStrokeWidth(1.2f * density);
        }

        @Override
        protected void onBoundsChange(Rect b) {
            float half = border.getStrokeWidth() / 2;
            box.set(b.left + half, b.top + half, b.right - half, b.bottom - half);
            border.setShader(new LinearGradient(b.right, b.top, b.left + b.width() * 0.35f, b.bottom,
                    0x8CFF2E9A, 0x00FF2E9A, Shader.TileMode.CLAMP));
        }

        @Override
        public void draw(Canvas canvas) {
            canvas.drawRoundRect(box, radius, radius, fill);
            canvas.drawRoundRect(box, radius, radius, border);
        }

        @Override
        public void getOutline(android.graphics.Outline outline) {
            outline.setRoundRect(getBounds(), radius);
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
