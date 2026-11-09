package com.tdisplay;

import android.net.Credentials;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Process;
import android.system.ErrnoException;
import android.system.OsConstants;
import android.util.Log;
import android.view.Surface;

import java.io.EOFException;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Waits for the laptop on a local socket that only adb can reach, and runs one session at a time. There is
 * no network code in this app: without the USB cable there is nothing that can connect.
 */
final class Link implements Runnable, VideoDecoder.Reader {
    private static final String TAG = "TDisplay";
    private static final long STATUS_INTERVAL_MS = 1000;
    private static final long QUIET_NS = 500_000_000L;
    /** Pings arrive every 100 ms, so this long without any message means something is stuck. */
    private static final long STALL_NS = 700_000_000L;

    interface Listener {
        /** Called with the screen state to show: a message, or null once frames are on screen. */
        void onLinkMessage(String message);
        void onStreamConfig(int width, int height, int fps);
        boolean isPowerSave();
        int thermalStatus();
        float refreshRate();
        float maxRefreshRate();
        int screenWidth();
        int screenHeight();
    }

    private final Surface surface;
    private final Listener listener;
    private final Thread thread;
    private volatile boolean closing;
    private volatile Outbox activeOutbox;
    private volatile LocalSocket activeSocket;
    private volatile long lastMessageNs;
    // Written by the status thread, picked up by the reader. Never taking a lock there means a
    // low-priority thread can't hold up the stream.
    private volatile boolean statusFresh;
    private volatile boolean statusPowerSave;
    private volatile int statusThermal;
    private volatile float statusRefreshHz;

    Link(Surface surface, Listener listener) {
        this.surface = surface;
        this.listener = listener;
        this.thread = new Thread(this, "tdisplay-link");
    }

    void start() {
        thread.start();
    }

    /** Current path for pen samples, or null when no session is running. */
    Outbox outbox() {
        return activeOutbox;
    }

    @Override
    public boolean isClosing() {
        return closing;
    }

    /** Ends everything, telling the laptop this was on purpose. Blocks until the decoder has let go of the surface. */
    void close() {
        closing = true;
        final Outbox outbox = activeOutbox;
        if (outbox != null) outbox.putGoodbye(Protocol.GOODBYE_APP_HIDDEN);
        final LocalSocket socket = activeSocket;
        if (socket != null) {
            try {
                if (outbox != null) outbox.finish(150);
                socket.shutdownInput();
                socket.shutdownOutput();
            } catch (IOException e) {
                Log.d(TAG, "socket shutdown", e);
            }
        } else {
            // Wake a blocked accept(); closing the server socket alone doesn't reliably do it.
            try (LocalSocket poke = new LocalSocket()) {
                poke.connect(new LocalSocketAddress(Protocol.SOCKET_NAME));
            } catch (IOException e) {
                Log.d(TAG, "accept wakeup", e);
            }
        }
        try {
            thread.join(2000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    @Override
    public void run() {
        Threads.raisePriority(Process.THREAD_PRIORITY_URGENT_DISPLAY);
        LocalServerSocket server = null;
        while (!closing && server == null) {
            try {
                server = new LocalServerSocket(Protocol.SOCKET_NAME);
            } catch (IOException e) {
                // The name stays busy for a moment after an earlier instance closes.
                try {
                    Thread.sleep(100);
                } catch (InterruptedException interrupted) {
                    return;
                }
            }
        }
        if (server == null) return;
        try {
            while (!closing) {
                listener.onLinkMessage("Waiting for your laptop.\nConnect the USB cable and start TDisplay on the laptop.");
                final LocalSocket socket = server.accept();
                final Credentials peer = socket.getPeerCredentials();
                if (peer.getUid() != Protocol.SHELL_UID || closing) {
                    socket.close();
                    continue;
                }
                serve(socket);
            }
        } catch (IOException e) {
            if (!closing) Log.e(TAG, "listening failed", e);
        } finally {
            try {
                server.close();
            } catch (IOException e) {
                Log.d(TAG, "server close", e);
            }
        }
    }

    /** Kicks the reader thread out of whatever it's blocked in, ending the session. */
    private void abortSession() {
        final LocalSocket socket = activeSocket;
        if (socket == null) return;
        try {
            socket.shutdownInput();
            socket.shutdownOutput();
        } catch (IOException e) {
            Log.d(TAG, "socket shutdown", e);
        }
    }

    private void serve(LocalSocket socket) {
        VideoDecoder decoder = null;
        Outbox outbox = null;
        Thread statusThread = null;
        Thread watchdog = null;
        activeSocket = socket;
        final long sessionStart = System.nanoTime();
        try {
            socket.setSoTimeout(Protocol.READ_TIMEOUT_MS);
            final InputStream in = socket.getInputStream();
            final FileDescriptor fd = socket.getFileDescriptor();
            outbox = new Outbox(socket.getOutputStream(), this::abortSession);
            outbox.start();
            activeOutbox = outbox;
            outbox.putHello(listener.screenWidth(), listener.screenHeight(), listener.maxRefreshRate(), android.os.Build.VERSION.SDK_INT);
            statusThread = startStatusThread();
            lastMessageNs = System.nanoTime();
            final Thread[] watched = {Thread.currentThread(), outbox, statusThread};
            watchdog = startWatchdog(watched);
            listener.onLinkMessage("Connecting");

            final byte[] small = new byte[4096];
            final ByteBuffer view = ByteBuffer.wrap(small).order(ByteOrder.LITTLE_ENDIAN);
            while (!closing) {
                readFully(in, small, 4);
                final long arrived = System.nanoTime();
                if (arrived - lastMessageNs > QUIET_NS) Log.w(TAG, "nothing arrived from the laptop for " + (arrived - lastMessageNs) / 1_000_000 + " ms");
                lastMessageNs = arrived;
                final int header = view.getInt(0);
                final int type = header >>> 24;
                final int length = header & 0x00FFFFFF;
                switch (type) {
                    case Protocol.FRAME: {
                        readFully(in, small, Protocol.FRAME_HEADER_SIZE);
                        final int frameId = view.getInt(0);
                        if (decoder == null) throw new IOException("frame before config");
                        Stats.frameIn(length);
                        decoder.queueFrame(fd, frameId, length - Protocol.FRAME_HEADER_SIZE, this);
                        break;
                    }
                    case Protocol.PING: {
                        readFully(in, small, Protocol.PING_SIZE);
                        outbox.putPong(view.getLong(0), System.nanoTime());
                        if (statusFresh) {
                            statusFresh = false;
                            outbox.putStatus(statusPowerSave, statusThermal, statusRefreshHz);
                        }
                        break;
                    }
                    case Protocol.CONFIG: {
                        if (length > small.length) throw new IOException("oversized config");
                        readFully(in, small, length);
                        final int width = view.getInt(0);
                        final int height = view.getInt(4);
                        final int fps = view.getInt(8);
                        final int codec = view.getInt(16);
                        final byte[] codecConfig = new byte[length - Protocol.CONFIG_SIZE];
                        System.arraycopy(small, Protocol.CONFIG_SIZE, codecConfig, 0, codecConfig.length);
                        listener.onStreamConfig(width, height, fps);
                        Stats.config(width, height, fps, codec);
                        if (decoder != null) decoder.stop();
                        decoder = new VideoDecoder(surface, outbox, new VideoDecoder.Listener() {
                            @Override
                            public void onFirstFrame() {
                                listener.onLinkMessage(null);
                            }

                            @Override
                            public void onDecoderFailed() {
                                abortSession();
                            }
                        });
                        decoder.start(width, height, fps, codec, codecConfig);
                        break;
                    }
                    case Protocol.BYE:
                        return;
                    default:
                        skip(in, small, length);
                        break;
                }
            }
        } catch (EOFException e) {
            Log.i(TAG, "laptop disconnected after " + (System.nanoTime() - sessionStart) / 1_000_000_000L + " s");
        } catch (ErrnoException e) {
            if (e.errno == OsConstants.EAGAIN) Log.w(TAG, "nothing from the laptop within the timeout");
            else if (!closing) Log.w(TAG, "socket read", e);
        } catch (IOException | IllegalStateException e) {
            if (!closing) Log.w(TAG, "session ended", e);
        } finally {
            activeOutbox = null;
            activeSocket = null;
            if (statusThread != null) statusThread.interrupt();
            if (watchdog != null) watchdog.interrupt();
            if (decoder != null) decoder.stop();
            if (outbox != null) outbox.finish(100);
            try {
                socket.close();
            } catch (IOException e) {
                Log.d(TAG, "socket close", e);
            }
        }
    }

    private static void appendStack(StringBuilder report, Thread thread) {
        report.append("\n").append(thread.getName()).append(" (").append(thread.getState()).append(')');
        final StackTraceElement[] stack = thread.getStackTrace();
        for (int i = 0; i < Math.min(stack.length, 12); i++) report.append("\n    ").append(stack[i]);
    }

    /** Logs where every thread is the moment the stream stalls, so a stall explains itself. */
    private Thread startWatchdog(Thread[] watched) {
        final Thread thread = new Thread(() -> {
            try {
                boolean logged = false;
                while (true) {
                    Thread.sleep(100);
                    final long quiet = System.nanoTime() - lastMessageNs;
                    if (quiet < STALL_NS) {
                        logged = false;
                    } else if (!logged) {
                        logged = true;
                        final StringBuilder report = new StringBuilder("stalled for ").append(quiet / 1_000_000).append(" ms:");
                        for (Thread t : watched) appendStack(report, t);
                        for (Thread t : Thread.getAllStackTraces().keySet()) {
                            if (t.getName().equals("tdisplay-decode") || t.getName().equals("tdisplay-gpu")) appendStack(report, t);
                        }
                        Log.w(TAG, report.toString());
                    }
                }
            } catch (InterruptedException e) {
                // The session is over.
            }
        }, "tdisplay-watchdog");
        thread.start();
        return thread;
    }

    /**
     * These questions go to Android's system services and can block for seconds when the system is busy,
     * so they get their own thread. On the reader thread they would stop the video and the pings too.
     */
    private Thread startStatusThread() {
        final Thread thread = new Thread(() -> {
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
            try {
                while (true) {
                    statusPowerSave = listener.isPowerSave();
                    statusThermal = listener.thermalStatus();
                    statusRefreshHz = listener.refreshRate();
                    statusFresh = true;
                    Thread.sleep(STATUS_INTERVAL_MS);
                }
            } catch (InterruptedException e) {
                // The session is over.
            }
        }, "tdisplay-status");
        thread.start();
        return thread;
    }

    private static void readFully(InputStream in, byte[] buffer, int length) throws IOException {
        int done = 0;
        while (done < length) {
            final int n = in.read(buffer, done, length - done);
            if (n < 0) throw new EOFException();
            done += n;
        }
    }

    private static void skip(InputStream in, byte[] buffer, int length) throws IOException {
        while (length > 0) {
            final int n = Math.min(length, buffer.length);
            readFully(in, buffer, n);
            length -= n;
        }
    }
}
