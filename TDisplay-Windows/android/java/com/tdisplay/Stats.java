package com.tdisplay;

import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Counters for the statistics overlay. The stream's threads only add to them, which costs an atomic add; the overlay
 * reads and clears them once a second. Nothing here is sent to the laptop.
 */
final class Stats {
    private static final AtomicLong bytesIn = new AtomicLong();
    private static final AtomicInteger framesIn = new AtomicInteger();
    private static final AtomicInteger framesShown = new AtomicInteger();
    private static final AtomicLong decodeUsSum = new AtomicLong();
    private static final AtomicInteger decodeUsMax = new AtomicInteger();
    private static final AtomicInteger skipped = new AtomicInteger();
    private static final AtomicInteger penSamples = new AtomicInteger();

    static volatile int width;
    static volatile int height;
    static volatile int fps;
    static volatile int codec = -1;
    /** Fingers on the screen now, palms included. UI thread. */
    static volatile int fingers;
    /** Largest contact seen since the last report, in millimeters and in Android's 0..1 size. UI thread. */
    static volatile float largestContactMm;
    static volatile float largestContactSize;

    private Stats() {}

    static void config(int w, int h, int f, int c) {
        width = w;
        height = h;
        fps = f;
        codec = c;
    }

    static void frameIn(int bytes) {
        bytesIn.addAndGet(bytes);
        framesIn.incrementAndGet();
    }

    static void frameShown(int decodeUs, int dropped) {
        framesShown.incrementAndGet();
        decodeUsSum.addAndGet(decodeUs);
        decodeUsMax.accumulateAndGet(decodeUs, Math::max);
        skipped.addAndGet(dropped);
    }

    static void penSample() {
        penSamples.incrementAndGet();
    }

    static void contact(float mm, float size) {
        if (mm > largestContactMm) largestContactMm = mm;
        if (size > largestContactSize) largestContactSize = size;
    }

    /** One second's worth, or however long since the last call. */
    static final class Report {
        double framesInPerSecond;
        double framesShownPerSecond;
        double mbps;
        double decodeMsMean;
        double decodeMsMax;
        int skipped;
        double penSamplesPerSecond;
        float largestContactMm;
        float largestContactSize;
    }

    static Report take(double seconds) {
        final Report r = new Report();
        final double s = Math.max(seconds, 0.001);
        final int in = framesIn.getAndSet(0);
        final int shown = framesShown.getAndSet(0);
        r.framesInPerSecond = in / s;
        r.framesShownPerSecond = shown / s;
        r.mbps = bytesIn.getAndSet(0) * 8.0 / s / 1e6;
        final long sum = decodeUsSum.getAndSet(0);
        r.decodeMsMean = shown > 0 ? sum / 1000.0 / shown : 0;
        r.decodeMsMax = decodeUsMax.getAndSet(0) / 1000.0;
        r.skipped = skipped.getAndSet(0);
        r.penSamplesPerSecond = penSamples.getAndSet(0) / s;
        r.largestContactMm = largestContactMm;
        r.largestContactSize = largestContactSize;
        largestContactMm = 0;
        largestContactSize = 0;
        return r;
    }
}
