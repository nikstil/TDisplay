package com.tdisplay;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.View;

/**
 * Drawn by the tablet on top of the laptop's picture: the instant cursor under the pen, and the statistics panel.
 * It takes no input; the activity hands all of that to the pen and touch code.
 */
final class OverlayView extends View {
    /** A pen held perfectly still sends nothing, so the cursor only goes after this long, or when the pen leaves. */
    private static final long CURSOR_TIMEOUT_MS = 2000;

    private final float density;
    private final Paint ringDark = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ringLight = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint dot = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint panel = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF panelRect = new RectF();
    private final Runnable cursorTimeout = this::hideCursor;

    private boolean cursorShown;
    private boolean cursorContact;
    private float cursorX;
    private float cursorY;
    private String[] statsLines;

    OverlayView(Context context) {
        super(context);
        density = context.getResources().getDisplayMetrics().density;
        ringDark.setStyle(Paint.Style.STROKE);
        ringDark.setStrokeWidth(3.5f * density);
        ringDark.setColor(Color.argb(170, 0, 0, 0));
        ringLight.setStyle(Paint.Style.STROKE);
        ringLight.setStrokeWidth(1.5f * density);
        ringLight.setColor(Color.WHITE);
        dot.setColor(Color.WHITE);
        panel.setColor(Color.argb(185, 0, 0, 0));
        text.setColor(Color.WHITE);
        text.setTypeface(Typeface.MONOSPACE);
        text.setTextSize(13f * context.getResources().getDisplayMetrics().scaledDensity);
        setWillNotDraw(false);
    }

    /** Where the pen is, in this view's pixels. UI thread. */
    void onPen(float x, float y, boolean contact) {
        if (!Prefs.cursor) {
            hideCursor();
            return;
        }
        cursorX = x;
        cursorY = y;
        cursorContact = contact;
        cursorShown = true;
        invalidate();
        removeCallbacks(cursorTimeout);
        postDelayed(cursorTimeout, CURSOR_TIMEOUT_MS);
    }

    void hideCursor() {
        removeCallbacks(cursorTimeout);
        if (!cursorShown) return;
        cursorShown = false;
        invalidate();
    }

    /** The statistics to show, or null to hide the panel. UI thread. */
    void setStats(String[] lines) {
        statsLines = lines;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (statsLines != null) {
            final float pad = 10 * density;
            final float lineHeight = text.getFontSpacing();
            float widest = 0;
            for (String line : statsLines) widest = Math.max(widest, text.measureText(line));
            final float left = 16 * density;
            final float top = 16 * density;
            panelRect.set(left, top, left + widest + 2 * pad, top + statsLines.length * lineHeight + 2 * pad);
            canvas.drawRoundRect(panelRect, 8 * density, 8 * density, panel);
            float y = top + pad - text.ascent();
            for (String line : statsLines) {
                canvas.drawText(line, left + pad, y, text);
                y += lineHeight;
            }
        }
        if (cursorShown) {
            final float radius = 6 * density;
            canvas.drawCircle(cursorX, cursorY, radius, ringDark);
            canvas.drawCircle(cursorX, cursorY, radius, ringLight);
            if (cursorContact) canvas.drawCircle(cursorX, cursorY, 1.6f * density, dot);
        }
    }
}
