package com.tdisplay;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.Settings;
import android.view.Display;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/** The full-screen display surface. Everything else lives in {@link Link}, {@link VideoDecoder} and {@link PenInput}. */
public final class DisplayActivity extends Activity implements SurfaceHolder.Callback, Link.Listener, PenInput.Target {
    private SurfaceView surfaceView;
    private TextView messageView;
    private PowerManager powerManager;
    private PenInput penInput;
    private TouchInput touchInput;
    private volatile Link link;
    private volatile float maxRefresh = 60f;
    private int screenWidth;
    private int screenHeight;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        powerManager = getSystemService(PowerManager.class);
        penInput = new PenInput(this);
        touchInput = new TouchInput(this);

        final Window window = getWindow();
        window.setDecorFitsSystemWindows(false);
        final WindowManager.LayoutParams attributes = window.getAttributes();
        attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        window.setAttributes(attributes);

        final Display display = getDisplay();
        final Display.Mode mode = display.getMode();
        screenWidth = Math.max(mode.getPhysicalWidth(), mode.getPhysicalHeight());
        screenHeight = Math.min(mode.getPhysicalWidth(), mode.getPhysicalHeight());
        for (Display.Mode m : display.getSupportedModes()) maxRefresh = Math.max(maxRefresh, m.getRefreshRate());
        requestRefreshRate(maxRefresh);

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        messageView = new TextView(this);
        messageView.setTextColor(Color.WHITE);
        messageView.setTextSize(20f);
        messageView.setGravity(Gravity.CENTER);
        messageView.setBackgroundColor(Color.rgb(16, 20, 24));

        final FrameLayout root = new FrameLayout(this);
        root.addView(surfaceView, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        root.addView(messageView, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        setContentView(root);

        // Deliver the pen as soon as each sample arrives instead of batching to the display's refresh.
        root.requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_POINTER);
        // The pen is drawn by the laptop; a second pointer from Android would just be in the way.
        root.setPointerIcon(PointerIcon.getSystemIcon(this, PointerIcon.TYPE_NULL));
        root.addOnLayoutChangeListener((v, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
            penInput.setViewSize(right - left, bottom - top);
            touchInput.setViewSize(right - left, bottom - top);
            // Keep edge swipes from turning into back gestures while the pen works near the sides.
            final List<Rect> edges = new ArrayList<>(2);
            edges.add(new Rect(0, 0, 200, bottom - top));
            edges.add(new Rect(right - left - 200, 0, right - left, bottom - top));
            v.setSystemGestureExclusionRects(edges);
        });
    }

    private void hideSystemBars() {
        final WindowInsetsController controller = getWindow().getInsetsController();
        if (controller == null) return;
        controller.hide(WindowInsets.Type.systemBars());
        controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    /** Nothing here reaches the tablet's own UI: the stylus goes to the laptop as a pen, and fingers as touches for gestures. */
    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        penInput.onEvent(event);
        touchInput.onEvent(event);
        return true;
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        penInput.onEvent(event);
        return true;
    }

    // SurfaceHolder.Callback: the session lives exactly as long as the surface, which goes away when
    // the app is hidden or the screen turns off.

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        final Surface surface = holder.getSurface();
        link = new Link(surface, this);
        link.start();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        final Link closing = link;
        link = null;
        if (closing != null) closing.close();
    }

    // PenInput.Target

    @Override
    public Outbox outbox() {
        final Link current = link;
        return current == null ? null : current.outbox();
    }

    // Link.Listener: called from the link and decoder threads.

    @Override
    public void onLinkMessage(String message) {
        runOnUiThread(() -> {
            if (message == null) {
                // The screen only stays on while it's really being used as a display, so a tablet
                // left unplugged on this screen still goes to sleep on its own.
                getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                messageView.setVisibility(View.GONE);
                return;
            }
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            String text = message;
            if (isPowerSave()) {
                text +="\n\nBattery Saver is on, which limits the screen to 60 Hz.\nTurn it off in Settings for high refresh.";
            }
            messageView.setText(text);
            messageView.setVisibility(View.VISIBLE);
        });
    }

    @Override
    public void onStreamConfig(int width, int height, int fps) {
        runOnUiThread(() -> {
            requestRefreshRate(fps);
            final SurfaceHolder holder = surfaceView.getHolder();
            holder.setFixedSize(width, height);
        });
    }

    /** Pins the panel to the slowest mode that can show every frame of the stream. */
    private void requestRefreshRate(float wanted) {
        final Display display = getDisplay();
        Display.Mode best = null;
        for (Display.Mode m : display.getSupportedModes()) {
            if (m.getRefreshRate() + 0.5f < wanted) continue;
            if (best == null || m.getRefreshRate() < best.getRefreshRate()) best = m;
        }
        if (best == null) return;
        final WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(attributes);
    }

    /**
     * Android calls Battery Saver off while the tablet is charging, yet still applies its 60 Hz cap if
     * the setting is on, so the setting is what matters here.
     */
    @Override
    public boolean isPowerSave() {
        if (powerManager.isPowerSaveMode()) return true;
        try {
            return Settings.Global.getInt(getContentResolver(), "low_power", 0) != 0;
        } catch (SecurityException e) {
            return false;
        }
    }

    @Override
    public int thermalStatus() {
        return powerManager.getCurrentThermalStatus();
    }

    @Override
    public float refreshRate() {
        final Display display = getDisplay();
        return display == null ? 0f : display.getRefreshRate();
    }

    @Override
    public float maxRefreshRate() {
        return maxRefresh;
    }

    @Override
    public int screenWidth() {
        return screenWidth;
    }

    @Override
    public int screenHeight() {
        return screenHeight;
    }
}
