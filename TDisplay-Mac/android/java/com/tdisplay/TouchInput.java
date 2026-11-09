package com.tdisplay;

import android.os.Build;
import android.view.MotionEvent;

/**
 * Forwards where the fingers are, for the laptop to turn two-finger movements into scrolling, zooming and rotating. One
 * finger, a palm, or any touch while the stylus is on or near the screen is left for the laptop to ignore; this only
 * reports. A laptop program that doesn't know about touches skips the message.
 */
final class TouchInput {
    private final PenInput.Target target;
    private final int[] ids = new int[Protocol.TOUCH_MAX_FINGERS];
    private final float[] xs = new float[Protocol.TOUCH_MAX_FINGERS];
    private final float[] ys = new float[Protocol.TOUCH_MAX_FINGERS];
    private int viewWidth = 1;
    private int viewHeight = 1;
    private boolean touching;

    TouchInput(PenInput.Target target) {
        this.target = target;
    }

    void setViewSize(int width, int height) {
        viewWidth = Math.max(width, 1);
        viewHeight = Math.max(height, 1);
    }

    private static boolean isFinger(MotionEvent event, int pointer) {
        final int tool = event.getToolType(pointer);
        return tool == MotionEvent.TOOL_TYPE_FINGER || tool == MotionEvent.TOOL_TYPE_UNKNOWN;
    }

    void onEvent(MotionEvent event) {
        final Outbox out = target.outbox();
        if (out == null) return;
        final int action = event.getActionMasked();
        // Hover and buttons belong to the stylus.
        if (action == MotionEvent.ACTION_HOVER_ENTER || action == MotionEvent.ACTION_HOVER_MOVE || action == MotionEvent.ACTION_HOVER_EXIT
                || action == MotionEvent.ACTION_BUTTON_PRESS || action == MotionEvent.ACTION_BUTTON_RELEASE) {
            return;
        }
        // A cancel, including the one Android sends when it decides a touch was a palm, ends whatever was going on.
        final boolean palm = Build.VERSION.SDK_INT >= 33 && (event.getFlags() & MotionEvent.FLAG_CANCELED) != 0;
        if (action == MotionEvent.ACTION_CANCEL || palm) {
            if (touching) out.putTouch(Protocol.TOUCH_CANCELED, ids, xs, ys, 0);
            touching = false;
            return;
        }
        // A finger that's lifting is still in the event; leave it out.
        final int lifting = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP ? event.getActionIndex() : -1;
        int count = 0;
        for (int i = 0; i < event.getPointerCount() && count < Protocol.TOUCH_MAX_FINGERS; i++) {
            if (i == lifting || !isFinger(event, i)) continue;
            ids[count] = event.getPointerId(i);
            xs[count] = event.getX(i) / viewWidth;
            ys[count] = event.getY(i) / viewHeight;
            count++;
        }
        if (count == 0 && !touching) return;
        touching = count > 0;
        out.putTouch(0, ids, xs, ys, count);
    }
}
