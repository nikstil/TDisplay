package com.tdisplay;

import android.os.Process;

final class Threads {
    /** Asks for a real-time-ish priority. Android may refuse, which only costs some latency. */
    static void raisePriority(int priority) {
        try {
            Process.setThreadPriority(priority);
        } catch (IllegalArgumentException | SecurityException e) {
            Thread.currentThread().setPriority(Thread.MAX_PRIORITY);
        }
    }

    private Threads() {}
}
