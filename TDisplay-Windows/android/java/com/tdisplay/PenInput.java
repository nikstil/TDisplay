package com.tdisplay;

import android.view.MotionEvent;

/**
 * Forwards the stylus and only the stylus. Fingers, palms and mice are never looked at beyond checking
 * what tool they are, so they can't move, click or draw anything on the laptop.
 */
final class PenInput {
    interface Target {
        Outbox outbox();
    }

    /** When the pen was last seen, touching or hovering, and whether it's near now. For the palm filter. */
    static volatile long lastPenNs;
    static volatile boolean penNear;

    private final Target target;
    private final OverlayView overlay;
    private int viewWidth = 1;
    private int viewHeight = 1;

    PenInput(Target target, OverlayView overlay) {
        this.target = target;
        this.overlay = overlay;
    }

    void setViewSize(int width, int height) {
        viewWidth = Math.max(width, 1);
        viewHeight = Math.max(height, 1);
    }

    private static int findStylus(MotionEvent event) {
        for (int i = 0; i < event.getPointerCount(); i++) {
            final int tool = event.getToolType(i);
            if (tool == MotionEvent.TOOL_TYPE_STYLUS || tool == MotionEvent.TOOL_TYPE_ERASER) return i;
        }
        return -1;
    }

    private static int toolAndButtonFlags(MotionEvent event, int pointer) {
        int flags = 0;
        final int buttons = event.getButtonState();
        if ((buttons & MotionEvent.BUTTON_STYLUS_PRIMARY) != 0) flags |= Protocol.PEN_BARREL;
        if (event.getToolType(pointer) == MotionEvent.TOOL_TYPE_ERASER || (buttons & MotionEvent.BUTTON_STYLUS_SECONDARY) != 0) {
            flags |= Protocol.PEN_ERASER;
        }
        return flags;
    }

    /** The screen reports lean as an angle plus a direction; Windows wants lean along each axis. */
    private static float tiltAlongX(float tilt, float orientation) {
        return (float) Math.toDegrees(Math.atan(Math.tan(tilt) * Math.sin(orientation)));
    }

    private static float tiltAlongY(float tilt, float orientation) {
        return (float) Math.toDegrees(Math.atan(-Math.tan(tilt) * Math.cos(orientation)));
    }

    void onEvent(MotionEvent event) {
        final int pointer = findStylus(event);
        if (pointer < 0) return;
        lastPenNs = System.nanoTime();

        final int action = event.getActionMasked();
        final int base = toolAndButtonFlags(event, pointer);
        final int state;
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE:
                state = Protocol.PEN_CONTACT | Protocol.PEN_IN_RANGE;
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
                if (event.getActionIndex() != pointer) return;
                state = Protocol.PEN_CONTACT | Protocol.PEN_IN_RANGE;
                break;
            case MotionEvent.ACTION_POINTER_UP:
                if (event.getActionIndex() != pointer) return;
                state = Protocol.PEN_IN_RANGE;
                break;
            case MotionEvent.ACTION_UP:
                state = Protocol.PEN_IN_RANGE;
                break;
            case MotionEvent.ACTION_CANCEL:
                state = Protocol.PEN_CANCELED;
                break;
            case MotionEvent.ACTION_HOVER_ENTER:
            case MotionEvent.ACTION_HOVER_MOVE:
                state = Protocol.PEN_IN_RANGE;
                break;
            case MotionEvent.ACTION_HOVER_EXIT:
                state = 0;
                break;
            case MotionEvent.ACTION_BUTTON_PRESS:
            case MotionEvent.ACTION_BUTTON_RELEASE:
                // A button change while touching or hovering: resend the current position with the new buttons.
                state = event.getPressure(pointer) > 0f ? Protocol.PEN_CONTACT | Protocol.PEN_IN_RANGE : Protocol.PEN_IN_RANGE;
                break;
            default:
                return;
        }
        penNear = state != 0 && state != Protocol.PEN_CANCELED;
        final Outbox out = target.outbox();
        if (out == null) {
            overlay.hideCursor();
            return;
        }
        GpuPresenter.stayReady();
        // The instant cursor goes first: it's the one thing here meant to be seen before the laptop has answered.
        if (penNear) overlay.onPen(event.getX(pointer), event.getY(pointer), (state & Protocol.PEN_CONTACT) != 0);
        else overlay.hideCursor();

        final int historical = event.getHistorySize();
        for (int h = 0; h < historical; h++) {
            final float tilt = event.getHistoricalAxisValue(MotionEvent.AXIS_TILT, pointer, h);
            final float orientation = event.getHistoricalOrientation(pointer, h);
            out.putPen(state | base, event.getHistoricalX(pointer, h) / viewWidth, event.getHistoricalY(pointer, h) / viewHeight,
                    event.getHistoricalPressure(pointer, h), tiltAlongX(tilt, orientation), tiltAlongY(tilt, orientation),
                    event.getHistoricalEventTime(h) * 1_000_000L);
            Stats.penSample();
        }
        final float tilt = event.getAxisValue(MotionEvent.AXIS_TILT, pointer);
        final float orientation = event.getOrientation(pointer);
        out.putPen(state | base, event.getX(pointer) / viewWidth, event.getY(pointer) / viewHeight, event.getPressure(pointer),
                tiltAlongX(tilt, orientation), tiltAlongY(tilt, orientation), event.getEventTime() * 1_000_000L);
        Stats.penSample();
    }
}
