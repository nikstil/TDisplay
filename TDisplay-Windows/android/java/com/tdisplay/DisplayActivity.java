package com.tdisplay;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.KeyguardManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.BatteryManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.Log;
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
import java.util.Locale;

/** The full-screen display surface. Everything else lives in {@link Link}, {@link VideoDecoder} and {@link PenInput}. */
public final class DisplayActivity extends Activity implements SurfaceHolder.Callback, Link.Listener, PenInput.Target {
    private static final long STATS_INTERVAL_MS = 1000;
    private static final String TAG = "TDisplay";
    private static final String MENU_HINT = "\n\nTap the bar at the top of the screen with a finger for the TDisplay menu.";

    private SurfaceView surfaceView;
    private FrameLayout root;
    private OverlayView overlay;
    private TabletMenu menu;
    private MenuHandle handle;
    /** The current touch began with a finger on the menu tab; it goes nowhere else until it lifts. */
    private boolean onHandle;
    private boolean handleTapSpoiled;
    /** App pinning was asked for during this visit, so a "No" isn't asked again until the app is left and reopened. */
    private boolean pinAsked;
    /** Pinning can't start over the lock screen, so it waits for the tablet to be unlocked. */
    private final BroadcastReceiver unlocked = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            pin();
        }
    };
    private HandlerThread statsThread;
    private Handler statsHandler;
    private long lastStatsNs;
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
        Prefs.load(this);
        final IntentFilter userPresent = new IntentFilter(Intent.ACTION_USER_PRESENT);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(unlocked, userPresent, Context.RECEIVER_EXPORTED);
        else registerReceiver(unlocked, userPresent);
        overlay = new OverlayView(this);
        penInput = new PenInput(this, overlay);
        touchInput = new TouchInput(this, getResources().getDisplayMetrics().xdpi / 25.4f);
        menu = new TabletMenu(this, new TabletMenu.Actions() {
            @Override
            public void onSettingsChanged() {
                applySettings();
            }

            @Override
            public void onLeave() {
                // The same as leaving any other way: the session ends and the laptop is told it was on purpose.
                unpin();
                moveTaskToBack(true);
            }
        });

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

        root = new FrameLayout(this);
        final FrameLayout.LayoutParams fill = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT);
        root.addView(surfaceView, fill);
        root.addView(overlay, new FrameLayout.LayoutParams(fill));
        root.addView(messageView, new FrameLayout.LayoutParams(fill));
        handle = new MenuHandle(this);
        root.addView(handle, new FrameLayout.LayoutParams(MenuHandle.touchWidthPx(this), MenuHandle.touchHeightPx(this),
                Gravity.TOP | Gravity.CENTER_HORIZONTAL));
        root.addView(menu, new FrameLayout.LayoutParams(fill));
        setContentView(root);

        // Deliver the pen as soon as each sample arrives instead of batching to the display's refresh.
        root.requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_POINTER);
        // The pen is drawn by the laptop; a second pointer from Android would just be in the way.
        root.setPointerIcon(PointerIcon.getSystemIcon(this, PointerIcon.TYPE_NULL));
        root.addOnLayoutChangeListener((v, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
            penInput.setViewSize(right - left, bottom - top);
            touchInput.setViewSize(right - left, bottom - top);
            applyEdgeLock(right - left, bottom - top);
        });
    }

    /**
     * Keeps edge swipes from turning into back gestures while the pen works near the sides. Android only honors part of
     * each edge, so with edge lock on, a back gesture that gets through anyway is ignored in onBackPressed.
     */
    private void applyEdgeLock(int width, int height) {
        final List<Rect> edges = new ArrayList<>(2);
        if (Prefs.edgeLock) {
            edges.add(new Rect(0, 0, 200, height));
            edges.add(new Rect(width - 200, 0, width, height));
        }
        root.setSystemGestureExclusionRects(edges);
    }

    private boolean isPinned() {
        final ActivityManager activities = getSystemService(ActivityManager.class);
        return activities != null && activities.getLockTaskModeState() != ActivityManager.LOCK_TASK_MODE_NONE;
    }

    /**
     * Android lets no app block the swipe up from the bottom edge, except through app pinning. Asking for it shows the
     * tablet's own "pin this app?" prompt, which only works once App pinning is turned on in the tablet's settings.
     */
    private void pin() {
        if (!Prefs.edgeLock || pinAsked || isPinned()) return;
        // Over the lock screen (after unpinning locked the tablet, say) Android ignores the request; ask once it's unlocked.
        final KeyguardManager keyguard = getSystemService(KeyguardManager.class);
        if (keyguard != null && keyguard.isKeyguardLocked()) return;
        pinAsked = true;
        try {
            startLockTask();
        } catch (RuntimeException e) {
            Log.w(TAG, "app pinning", e);
        }
    }

    private void unpin() {
        if (!isPinned()) return;
        try {
            stopLockTask();
        } catch (RuntimeException e) {
            Log.w(TAG, "app unpinning", e);
        }
    }

    /** After a switch in the menu changes. */
    private void applySettings() {
        applyEdgeLock(root.getWidth(), root.getHeight());
        if (Prefs.edgeLock) {
            pinAsked = false;
            pin();
        } else {
            unpin();
        }
        if (!Prefs.cursor) overlay.hideCursor();
        if (Prefs.stats) startStats();
        else stopStats();
    }

    private void openMenu() {
        touchInput.suppress();
        overlay.hideCursor();
        menu.show();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (menu.isOpen()) {
            menu.hide();
            return;
        }
        if (Prefs.edgeLock) return;
        super.onBackPressed();
    }

    private void startStats() {
        if (statsThread != null) return;
        statsThread = new HandlerThread("tdisplay-stats", android.os.Process.THREAD_PRIORITY_BACKGROUND);
        statsThread.start();
        statsHandler = new Handler(statsThread.getLooper());
        lastStatsNs = System.nanoTime();
        Stats.take(1);
        statsHandler.postDelayed(this::refreshStats, STATS_INTERVAL_MS);
    }

    private void stopStats() {
        if (statsThread == null) return;
        statsThread.quitSafely();
        statsThread = null;
        statsHandler = null;
        overlay.setStats(null);
    }

    /** On the stats thread: the battery and thermal questions go to system services, which can be slow. */
    private void refreshStats() {
        final Handler handler = statsHandler;
        if (handler == null) return;
        final long now = System.nanoTime();
        final Stats.Report r = Stats.take((now - lastStatsNs) / 1e9);
        lastStatsNs = now;
        final BatteryManager battery = getSystemService(BatteryManager.class);
        final int percent = battery.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY);
        final int currentMa = Math.abs(battery.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)) / 1000;
        final String codec = Stats.codec == Protocol.CODEC_H264 ? "H.264" : Stats.codec == Protocol.CODEC_HEVC ? "HEVC" : "-";
        final String[] lines = {
            String.format(Locale.US, "Stream   %dx%d %s, %d fps, %.1f Mbps", Stats.width, Stats.height, codec, Stats.fps, r.mbps),
            String.format(Locale.US, "Frames   %.0f/s in, %.0f/s shown, %d skipped", r.framesInPerSecond, r.framesShownPerSecond, r.skipped),
            String.format(Locale.US, "Decode   %.1f ms, max %.1f", r.decodeMsMean, r.decodeMsMax),
            String.format(Locale.US, "Screen   %.0f Hz, %s", refreshRate(), VideoDecoder.presentThroughGpu ? "drawn by the GPU" : "straight from the decoder"),
            String.format(Locale.US, "Pen      %.0f samples/s", r.penSamplesPerSecond),
            String.format(Locale.US, "Touch    %d down, largest %.1f mm (size %.3f)", Stats.fingers, r.largestContactMm, r.largestContactSize),
            String.format(Locale.US, "Battery  %d%%, %s %d mA, thermal %d%s", percent, battery.isCharging() ? "charging" : "draining", currentMa,
                    thermalStatus(), isPowerSave() ? ", Battery Saver on" : ""),
        };
        overlay.post(() -> {
            if (statsThread != null) overlay.setStats(lines);
        });
        handler.postDelayed(this::refreshStats, STATS_INTERVAL_MS);
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
        if (Prefs.stats) startStats();
        pin();
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(unlocked);
        super.onDestroy();
    }

    @Override
    protected void onStop() {
        super.onStop();
        pinAsked = false;
    }

    @Override
    protected void onPause() {
        super.onPause();
        stopStats();
        menu.hide();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    /** Nothing here reaches the tablet's own UI: the stylus goes to the laptop as a pen, and fingers as touches for gestures. */
    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        // While the menu is open it gets everything, and the laptop nothing.
        if (menu.isOpen()) return super.dispatchTouchEvent(event);
        if (handleTouch(event)) return true;
        penInput.onEvent(event);
        touchInput.onEvent(event);
        return true;
    }

    /** A finger tap on the menu tab opens the menu. The pen, and fingers anywhere else, carry on as normal. */
    private boolean handleTouch(MotionEvent event) {
        final int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            onHandle = event.getToolType(0) == MotionEvent.TOOL_TYPE_FINGER && handle.contains(event.getX(), event.getY());
            handleTapSpoiled = false;
        }
        if (!onHandle) return false;
        if (action == MotionEvent.ACTION_POINTER_DOWN || action == MotionEvent.ACTION_CANCEL) handleTapSpoiled = true;
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            onHandle = false;
            if (!handleTapSpoiled && handle.contains(event.getX(), event.getY())) openMenu();
        }
        return true;
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (menu.isOpen()) return super.dispatchGenericMotionEvent(event);
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
            messageView.setText(text + MENU_HINT);
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
