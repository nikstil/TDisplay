package com.tdisplay;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.View;

/**
 * The small bar at the top of the screen that opens the tablet menu when tapped with a finger. The pen goes straight
 * through it to the laptop, and the activity does the hit test, so this only draws.
 */
final class MenuHandle extends View {
    private final float density;
    private final Paint outline = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF bar = new RectF();

    MenuHandle(Context context) {
        super(context);
        density = context.getResources().getDisplayMetrics().density;
        outline.setColor(Color.argb(110, 0, 0, 0));
        fill.setColor(Color.argb(150, 255, 255, 255));
    }

    /** The area a finger can tap, larger than the bar itself. */
    static int touchWidthPx(Context context) {
        return Math.round(160 * context.getResources().getDisplayMetrics().density);
    }

    static int touchHeightPx(Context context) {
        return Math.round(40 * context.getResources().getDisplayMetrics().density);
    }

    boolean contains(float x, float y) {
        return x >= getLeft() && x < getRight() && y >= getTop() && y < getBottom();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        final float width = 64 * density;
        final float height = 5 * density;
        final float left = (getWidth() - width) / 2;
        final float top = 8 * density;
        final float edge = 1 * density;
        bar.set(left - edge, top - edge, left + width + edge, top + height + edge);
        canvas.drawRoundRect(bar, height, height, outline);
        bar.set(left, top, left + width, top + height);
        canvas.drawRoundRect(bar, height / 2, height / 2, fill);
    }
}
