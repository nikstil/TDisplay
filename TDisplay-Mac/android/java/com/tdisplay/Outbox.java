package com.tdisplay;

import android.os.Process;
import android.util.Log;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Sends everything that goes to the laptop from one thread, so that the UI thread (pen input) and the
 * decoder thread (acks) never block on the socket. Allocates nothing per message.
 */
final class Outbox extends Thread {
    private static final int CAPACITY = 1 << 16;
    private static final int RECORD_MAX = 64;
    private static final long SLOW_WRITE_NS = 250_000_000L;

    private final OutputStream out;
    private final Runnable onFailure;
    private final byte[] ring = new byte[CAPACITY];
    private final byte[] chunk = new byte[CAPACITY];
    private final ByteBuffer scratch = ByteBuffer.allocate(RECORD_MAX).order(ByteOrder.LITTLE_ENDIAN);
    private int head;
    private int size;
    private boolean closing;

    /** onFailure runs if sending fails: without this thread the laptop hears nothing, so the session has to end. */
    Outbox(OutputStream out, Runnable onFailure) {
        super("tdisplay-outbox");
        this.out = out;
        this.onFailure = onFailure;
    }

    @Override
    public void run() {
        Threads.raisePriority(Process.THREAD_PRIORITY_URGENT_DISPLAY);
        try {
            while (true) {
                int n;
                synchronized (this) {
                    while (size == 0 && !closing) wait();
                    if (size == 0) return;
                    n = size;
                    final int first = Math.min(n, CAPACITY - head);
                    System.arraycopy(ring, head, chunk, 0, first);
                    System.arraycopy(ring, 0, chunk, first, n - first);
                    head = (head + n) % CAPACITY;
                    size = 0;
                }
                final long started = System.nanoTime();
                out.write(chunk, 0, n);
                final long took = System.nanoTime() - started;
                if (took > SLOW_WRITE_NS) Log.w("TDisplay", "sending to the laptop blocked for " + took / 1_000_000 + " ms");
            }
        } catch (IOException | InterruptedException e) {
            Log.w("TDisplay", "sending to the laptop stopped", e);
            onFailure.run();
        }
    }

    /** Sends whatever is queued, then stops. */
    void finish(long waitMs) {
        synchronized (this) {
            closing = true;
            notifyAll();
        }
        try {
            join(waitMs);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private void begin(int type, int length) {
        scratch.clear();
        scratch.putInt((type << 24) | length);
    }

    private void commit() {
        final int n = scratch.position();
        if (size + n > CAPACITY) return;  // The link has stalled; dropping beats blocking the caller.
        int tail = (head + size) % CAPACITY;
        final int first = Math.min(n, CAPACITY - tail);
        System.arraycopy(scratch.array(), 0, ring, tail, first);
        System.arraycopy(scratch.array(), first, ring, 0, n - first);
        size += n;
        notifyAll();
    }

    synchronized void putHello(int width, int height, float maxRefreshHz, int sdk) {
        begin(Protocol.HELLO, 20);
        scratch.putInt(Protocol.VERSION).putInt(width).putInt(height).putFloat(maxRefreshHz).putInt(sdk);
        commit();
    }

    synchronized void putPen(int flags, float x, float y, float pressure, float tiltX, float tiltY, long timeNs) {
        begin(Protocol.PEN, 32);
        scratch.putInt(flags).putFloat(x).putFloat(y).putFloat(pressure).putFloat(tiltX).putFloat(tiltY).putLong(timeNs);
        commit();
    }

    synchronized void putAck(int frameId, int decodeUs, int droppedFrames) {
        begin(Protocol.ACK, 12);
        scratch.putInt(frameId).putInt(decodeUs).putInt(droppedFrames);
        commit();
    }

    synchronized void putPong(long laptopUs, long tabletNs) {
        begin(Protocol.PONG, 16);
        scratch.putLong(laptopUs).putLong(tabletNs);
        commit();
    }

    synchronized void putGoodbye(int reason) {
        begin(Protocol.GOODBYE, 4);
        scratch.putInt(reason);
        commit();
    }

    synchronized void putTouch(int flags, int[] ids, float[] xs, float[] ys, int count) {
        begin(Protocol.TOUCH, 4 + count * 12);
        scratch.putInt(flags | count);
        for (int i = 0; i < count; i++) scratch.putInt(ids[i]).putFloat(xs[i]).putFloat(ys[i]);
        commit();
    }

    synchronized void putStatus(boolean powerSave, int thermal, float refreshHz) {
        begin(Protocol.STATUS, 12);
        scratch.putInt(powerSave ? 1 : 0).putInt(thermal).putFloat(refreshHz);
        commit();
    }
}
