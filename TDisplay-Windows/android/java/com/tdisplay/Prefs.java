package com.tdisplay;

import android.content.Context;
import android.content.SharedPreferences;

/** The tablet menu's switches, remembered on the tablet. Read from any thread. */
final class Prefs {
    /** A dot under the pen, drawn by the tablet ahead of the laptop's picture. */
    static volatile boolean cursor = true;
    /** Frame rate, delay, battery and touch sizes in a corner. */
    static volatile boolean stats = false;
    /** Edge swipes can't take the app away. */
    static volatile boolean edgeLock = true;
    /** Palms, and hands that landed while the pen was near, never count as gesture fingers. */
    static volatile boolean palmFilter = true;

    private static SharedPreferences store;

    private Prefs() {}

    static void load(Context context) {
        store = context.getSharedPreferences("tdisplay", Context.MODE_PRIVATE);
        cursor = store.getBoolean("cursor", cursor);
        stats = store.getBoolean("stats", stats);
        edgeLock = store.getBoolean("edgeLock", edgeLock);
        palmFilter = store.getBoolean("palmFilter", palmFilter);
    }

    static void save() {
        if (store == null) return;
        store.edit()
                .putBoolean("cursor", cursor)
                .putBoolean("stats", stats)
                .putBoolean("edgeLock", edgeLock)
                .putBoolean("palmFilter", palmFilter)
                .apply();
    }
}
