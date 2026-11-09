package com.tdisplay;

import android.os.Build;
import android.util.SparseArray;
import android.view.MotionEvent;

/**
 * Forwards where the fingers are, for the laptop to turn two fingers into touchscreen gestures.
 *
 * Beyond the Mac version's touch reporting, this filters on the tablet: with the palm filter on, a contact the size of
 * a palm, or any finger that touched down while the pen was near or had just left, is never reported. Up to four
 * fingers are reported, so the laptop can tell a three-finger tap (redo) and call off a two-finger gesture when a third
 * finger joins.
 */
final class TouchInput {
    /** A finger that lands this soon after the pen was last seen belongs to the hand holding it. */
    private static final long PEN_QUIET_NS = 500_000_000L;
    /** Bigger than any fingertip or thumb, smaller than a palm or the side of a hand. */
    private static final float PALM_MM = 20f;

    private final PenInput.Target target;
    private final float pxPerMm;
    private final int[] ids = new int[Protocol.TOUCH_MAX_FINGERS];
    private final float[] xs = new float[Protocol.TOUCH_MAX_FINGERS];
    private final float[] ys = new float[Protocol.TOUCH_MAX_FINGERS];
    /** Pointer ids that never count, until they lift. */
    private final SparseArray<Boolean> ignored = new SparseArray<>();
    private int viewWidth = 1;
    private int viewHeight = 1;
    private boolean touching;
    private boolean suppressed;

    TouchInput(PenInput.Target target, float pxPerMm) {
        this.target = target;
        this.pxPerMm = Math.max(pxPerMm, 1f);
    }

    void setViewSize(int width, int height) {
        viewWidth = Math.max(width, 1);
        viewHeight = Math.max(height, 1);
    }

    private static boolean isFinger(MotionEvent event, int pointer) {
        final int tool = event.getToolType(pointer);
        return tool == MotionEvent.TOOL_TYPE_FINGER || tool == MotionEvent.TOOL_TYPE_UNKNOWN;
    }

    private boolean penRecent() {
        return PenInput.penNear || System.nanoTime() - PenInput.lastPenNs < PEN_QUIET_NS;
    }

    private boolean palmSized(MotionEvent event, int pointer) {
        return event.getTouchMajor(pointer) / pxPerMm > PALM_MM;
    }

    /** Stops reporting until every finger lifts, ending anything the laptop thought was going on as canceled. */
    private void stopReporting() {
        suppressed = true;
        final Outbox out = target.outbox();
        if (touching && out != null) out.putTouch(Protocol.TOUCH_CANCELED, ids, xs, ys, 0);
        touching = false;
    }

    /** The same, for the activity when the menu opens. */
    void suppress() {
        stopReporting();
    }

    private void resetAll() {
        ignored.clear();
        suppressed = false;
        Stats.fingers = 0;
    }

    void onEvent(MotionEvent event) {
        final int action = event.getActionMasked();
        // Hover and buttons belong to the stylus.
        if (action == MotionEvent.ACTION_HOVER_ENTER || action == MotionEvent.ACTION_HOVER_MOVE || action == MotionEvent.ACTION_HOVER_EXIT
                || action == MotionEvent.ACTION_BUTTON_PRESS || action == MotionEvent.ACTION_BUTTON_RELEASE) {
            return;
        }
        // A cancel, including the one Android sends when it decides a touch was a palm, ends whatever was going on.
        final boolean palmCancel = Build.VERSION.SDK_INT >= 33 && (event.getFlags() & MotionEvent.FLAG_CANCELED) != 0;
        if (action == MotionEvent.ACTION_CANCEL) {
            suppress();
            resetAll();
            return;
        }
        if (palmCancel) {
            // Android took back one contact as a palm: the rest of this touch doesn't count either.
            suppress();
            if (action == MotionEvent.ACTION_UP) resetAll();
            else if (action == MotionEvent.ACTION_POINTER_UP) ignored.remove(event.getPointerId(event.getActionIndex()));
            return;
        }
        if (action == MotionEvent.ACTION_DOWN) {
            // The first contact of a new touch: nothing from before still applies.
            stopReporting();
            resetAll();
        }

        // Decide about a finger as it lands: one that comes down near the pen is the hand holding it.
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            final int index = event.getActionIndex();
            if (isFinger(event, index) && Prefs.palmFilter && (penRecent() || palmSized(event, index))) {
                ignored.put(event.getPointerId(index), Boolean.TRUE);
            }
        }

        final int lifting = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP ? event.getActionIndex() : -1;
        int count = 0;
        int down = 0;
        boolean palmAppeared = false;
        for (int i = 0; i < event.getPointerCount(); i++) {
            if (i == lifting || !isFinger(event, i)) continue;
            down++;
            Stats.contact(event.getTouchMajor(i) / pxPerMm, event.getSize(i));
            final int id = event.getPointerId(i);
            if (ignored.get(id) != null) continue;
            // A finger that spreads to palm size, like a hand settling onto the screen, stops counting.
            if (Prefs.palmFilter && palmSized(event, i)) {
                ignored.put(id, Boolean.TRUE);
                palmAppeared = true;
                continue;
            }
            if (count < Protocol.TOUCH_MAX_FINGERS) {
                ids[count] = id;
                xs[count] = event.getX(i) / viewWidth;
                ys[count] = event.getY(i) / viewHeight;
                count++;
            }
        }
        Stats.fingers = down;

        if (palmAppeared && touching) stopReporting();

        if (action == MotionEvent.ACTION_UP) {
            // Every finger is off the screen: start afresh.
            final Outbox out = target.outbox();
            if (touching && !suppressed && out != null) out.putTouch(0, ids, xs, ys, 0);
            touching = false;
            resetAll();
            return;
        }
        if (action == MotionEvent.ACTION_POINTER_UP) ignored.remove(event.getPointerId(event.getActionIndex()));
        if (suppressed) return;

        final Outbox out = target.outbox();
        if (out == null) return;
        if (count == 0 && !touching) return;
        touching = count > 0;
        out.putTouch(0, ids, xs, ys, count);
    }
}
