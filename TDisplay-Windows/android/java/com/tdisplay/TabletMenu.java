package com.tdisplay;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.ContextThemeWrapper;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

/**
 * The tablet's own menu, opened by tapping the bar at the top of the screen with a finger. While it's open, the pen
 * and fingers work the menu and nothing reaches the laptop.
 */
final class TabletMenu extends FrameLayout {
    interface Actions {
        void onSettingsChanged();
        void onLeave();
    }

    /** Sets one of the switches. */
    private interface Setter {
        void set(boolean on);
    }

    private final float density;

    TabletMenu(Context base, Actions actions) {
        super(new ContextThemeWrapper(base, android.R.style.Theme_DeviceDefault));
        final Context context = getContext();
        density = context.getResources().getDisplayMetrics().density;
        setBackgroundColor(Color.argb(150, 0, 0, 0));
        setVisibility(GONE);
        // A tap beside the panel closes the menu.
        setOnClickListener(v -> hide());

        final LinearLayout panel = new LinearLayout(context);
        panel.setOrientation(LinearLayout.VERTICAL);
        final int pad = dp(28);
        panel.setPadding(pad, dp(22), pad, dp(18));
        final GradientDrawable background = new GradientDrawable();
        background.setColor(Color.rgb(32, 36, 42));
        background.setCornerRadius(dp(20));
        panel.setBackground(background);
        panel.setClickable(true);

        final TextView title = new TextView(context);
        title.setText("TDisplay");
        title.setTextSize(24f);
        title.setTextColor(Color.WHITE);
        panel.addView(title);

        addSwitch(panel, "Instant cursor", "A dot under the pen tip, drawn by the tablet straight away while the laptop's picture catches up.",
                Prefs.cursor, on -> Prefs.cursor = on, actions);
        addSwitch(panel, "Palm filter", "A palm, or a hand that touched down while the pen was near, never counts as a gesture finger.",
                Prefs.palmFilter, on -> Prefs.palmFilter = on, actions);
        addSwitch(panel, "Edge lock", "Pins TDisplay so no edge swipe can take you out of it. Turn on App pinning once in the tablet's Settings (search for it), then confirm the tablet's prompt. Leave with the button below.",
                Prefs.edgeLock, on -> Prefs.edgeLock = on, actions);
        addSwitch(panel, "Statistics", "Frame rate, decoding time, battery and touch sizes in the top-left corner.",
                Prefs.stats, on -> Prefs.stats = on, actions);

        final LinearLayout buttons = new LinearLayout(context);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.END);
        final Button leave = new Button(context);
        leave.setText("Leave TDisplay");
        leave.setOnClickListener(v -> {
            hide();
            actions.onLeave();
        });
        final Button done = new Button(context);
        done.setText("Done");
        done.setOnClickListener(v -> hide());
        buttons.addView(leave);
        buttons.addView(done);
        final LinearLayout.LayoutParams buttonRow = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        buttonRow.topMargin = dp(18);
        panel.addView(buttons, buttonRow);

        final TextView hint = new TextView(context);
        hint.setText("Tap the bar at the top of the screen with a finger to open this menu again.");
        hint.setTextSize(13f);
        hint.setTextColor(Color.rgb(150, 156, 164));
        final LinearLayout.LayoutParams hintRow = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        hintRow.topMargin = dp(10);
        panel.addView(hint, hintRow);

        addView(panel, new FrameLayout.LayoutParams(dp(560), FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
    }

    private int dp(float value) {
        return Math.round(value * density);
    }

    private void addSwitch(LinearLayout panel, String name, String description, boolean on, Setter setter, Actions actions) {
        final Context context = getContext();
        final Switch toggle = new Switch(context);
        toggle.setText(name);
        toggle.setTextSize(19f);
        toggle.setTextColor(Color.WHITE);
        toggle.setChecked(on);
        toggle.setOnCheckedChangeListener((button, checked) -> {
            setter.set(checked);
            Prefs.save();
            actions.onSettingsChanged();
        });
        final LinearLayout.LayoutParams switchRow = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        switchRow.topMargin = dp(20);
        panel.addView(toggle, switchRow);

        final TextView about = new TextView(context);
        about.setText(description);
        about.setTextSize(14f);
        about.setTextColor(Color.rgb(170, 176, 184));
        panel.addView(about);
    }

    boolean isOpen() {
        return getVisibility() == VISIBLE;
    }

    void show() {
        setVisibility(VISIBLE);
    }

    void hide() {
        setVisibility(GONE);
    }
}
