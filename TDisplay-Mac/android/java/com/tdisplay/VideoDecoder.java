package com.tdisplay;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Process;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.Surface;

import java.io.ByteArrayOutputStream;
import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.ByteBuffer;

/** Hardware HEVC or H.264 decoding straight to the screen with as little buffering as the decoder allows. */
final class VideoDecoder {
    private static final String TAG = "TDisplay";
    private static final int MAX_FRAME_BYTES = 8 * 1024 * 1024;
    private static final int TIMING_SLOTS = 256;
    /** A decoder that accepts no input for this long is stuck. */
    private static final long INPUT_STALL_NS = 1_500_000_000L;
    private static final long SLOW_NS = 250_000_000L;

    interface Listener {
        void onFirstFrame();
        /** The decoder has died, and the session can't continue. */
        void onDecoderFailed();
    }

    private final Surface surface;
    private final Outbox outbox;
    private final Listener listener;
    private final long[] queuedAtNs = new long[TIMING_SLOTS];

    /** Other chips show the decoder's pictures directly. If the GPU fails, the direct route takes over until the app restarts. */
    static volatile boolean presentThroughGpu = isMediaTek();

    private String mime = MediaFormat.MIMETYPE_VIDEO_HEVC;
    private MediaCodec codec;
    private GpuPresenter presenter;
    private Thread outputThread;
    private volatile boolean running;

    VideoDecoder(Surface surface, Outbox outbox, Listener listener) {
        this.surface = surface;
        this.outbox = outbox;
        this.listener = listener;
    }

    private static boolean isMediaTek() {
        return (Build.VERSION.SDK_INT >= 31 && "mediatek".equalsIgnoreCase(Build.SOC_MANUFACTURER)) || Build.HARDWARE.toLowerCase().startsWith("mt");
    }

    /** The low-latency variant of the hardware decoder if the tablet has one. */
    private static String pickDecoder(String mime) {
        String fallback = null;
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (info.isEncoder() || !info.isHardwareAccelerated()) continue;
            for (String type : info.getSupportedTypes()) {
                if (!type.equalsIgnoreCase(mime)) continue;
                final MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                if (info.getName().contains("secure")) continue;
                if (caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)) return info.getName();
                if (fallback == null) fallback = info.getName();
            }
        }
        return fallback;
    }

    /**
     * H.264 decoders take the sequence and picture parameter sets separately, in csd-0 and csd-1. They arrive
     * together, each after a start code.
     */
    private static ByteBuffer[] splitH264Config(byte[] config) {
        final ByteArrayOutputStream[] sets = {new ByteArrayOutputStream(), new ByteArrayOutputStream()};
        int start = -1;
        for (int i = 0; i <= config.length; i++) {
            final boolean startCode = i + 3 <= config.length && config[i] == 0 && config[i + 1] == 0 && config[i + 2] == 1;
            if (!startCode && i < config.length) continue;
            if (start >= 0) {
                int end = i;
                while (end > start && config[end - 1] == 0) end--;
                final int type = end > start ? config[start] & 0x1F : 0;
                if (type == 7 || type == 8) {
                    final ByteArrayOutputStream set = sets[type == 7 ? 0 : 1];
                    set.write(new byte[] {0, 0, 0, 1}, 0, 4);
                    set.write(config, start, end - start);
                }
            }
            start = i + 3;
            i += 2;
        }
        return new ByteBuffer[] {ByteBuffer.wrap(sets[0].toByteArray()), ByteBuffer.wrap(sets[1].toByteArray())};
    }

    private MediaFormat buildFormat(String decoder, int width, int height, int fps, byte[] codecConfig, int attempt) {
        final MediaFormat format = MediaFormat.createVideoFormat(mime, width, height);
        if (mime.equals(MediaFormat.MIMETYPE_VIDEO_AVC)) {
            final ByteBuffer[] sets = splitH264Config(codecConfig);
            format.setByteBuffer("csd-0", sets[0]);
            format.setByteBuffer("csd-1", sets[1]);
        } else {
            format.setByteBuffer("csd-0", ByteBuffer.wrap(codecConfig));
        }
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, MAX_FRAME_BYTES);
        format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
        format.setInteger(MediaFormat.KEY_PRIORITY, 0);
        format.setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709);
        format.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED);
        format.setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO);
        if (attempt == 0) {
            // Some firmware reads its maker's own name instead of the official key.
            if (decoder.contains("mtk")) format.setInteger("vdec-lowlatency", 1);
            if (decoder.contains("qti") || decoder.contains("qcom")) {
                format.setInteger("vendor.qti-ext-dec-low-latency.enable", 1);
                format.setInteger("vendor.qti-ext-dec-picture-order.enable", 1);
            }
            if (decoder.contains("exynos")) format.setInteger("vendor.rtc-ext-dec-low-latency.enable", 1);
            format.setInteger(MediaFormat.KEY_OPERATING_RATE, Short.MAX_VALUE);
        } else if (attempt == 1) {
            format.setInteger(MediaFormat.KEY_OPERATING_RATE, fps);
        }
        return format;
    }

    void start(int width, int height, int fps, int videoCodec, byte[] codecConfig) throws IOException {
        mime = videoCodec == Protocol.CODEC_H264 ? MediaFormat.MIMETYPE_VIDEO_AVC : MediaFormat.MIMETYPE_VIDEO_HEVC;
        final String name = pickDecoder(mime);
        if (name == null) throw new IOException("no hardware decoder for " + mime);
        Surface target = surface;
        if (presentThroughGpu) {
            presenter = new GpuPresenter(surface, width, height, () -> {
                presentThroughGpu = false;
                listener.onDecoderFailed();
            });
            try {
                target = presenter.start();
            } catch (IOException e) {
                Log.w(TAG, "going straight to the screen instead of through the GPU", e);
                presentThroughGpu = false;
                presenter.stop();
                presenter = null;
            }
        }
        Exception last = null;
        for (int attempt = 0; attempt < 3 && codec == null; attempt++) {
            MediaCodec candidate = null;
            try {
                candidate = MediaCodec.createByCodecName(name);
                candidate.configure(buildFormat(name.toLowerCase(), width, height, fps, codecConfig, attempt), target, null, 0);
                candidate.start();
                codec = candidate;
                Log.i(TAG, "decoder " + name + " started on attempt " + attempt);
            } catch (IllegalStateException | IllegalArgumentException e) {
                last = e;
                Log.w(TAG, "decoder configuration attempt " + attempt + " failed", e);
                if (candidate != null) candidate.release();
            }
        }
        if (codec == null) throw new IOException("decoder would not start", last);

        running = true;
        outputThread = new Thread(this::drainOutput, "tdisplay-decode");
        outputThread.start();
    }

    void stop() {
        running = false;
        if (outputThread != null) {
            try {
                outputThread.join(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            outputThread = null;
        }
        if (codec != null) {
            try {
                codec.stop();
            } catch (IllegalStateException e) {
                Log.w(TAG, "decoder stop", e);
            }
            codec.release();
            codec = null;
        }
        if (presenter != null) {
            presenter.stop();
            presenter = null;
        }
    }

    /** Reads a frame's bytes from the socket straight into the decoder's own buffer. */
    void queueFrame(FileDescriptor socket, int frameId, int length, Reader reader) throws IOException, ErrnoException {
        final long waitStart = System.nanoTime();
        int index;
        while ((index = codec.dequeueInputBuffer(100_000)) < 0) {
            if (reader.isClosing()) throw new IOException("closing");
            if (System.nanoTime() - waitStart > INPUT_STALL_NS) throw new IOException("decoder stopped accepting input");
        }
        final long waited = System.nanoTime() - waitStart;
        if (waited > SLOW_NS) Log.w(TAG, "decoder took " + waited / 1_000_000 + " ms to accept a frame");
        final ByteBuffer input = codec.getInputBuffer(index);
        if (input == null || length > input.capacity()) {
            codec.queueInputBuffer(index, 0, 0, frameId, 0);
            throw new IOException("frame of " + length + " bytes does not fit the decoder buffer");
        }
        input.clear();
        input.limit(length);
        while (input.hasRemaining()) {
            final int n = Os.read(socket, input);
            if (n <= 0) throw new IOException("connection closed mid-frame");
        }
        queuedAtNs[frameId % TIMING_SLOTS] = System.nanoTime();
        codec.queueInputBuffer(index, 0, length, frameId, 0);
    }

    /** Implemented by the socket reader so the decoder can tell when to give up waiting. */
    interface Reader {
        boolean isClosing();
    }

    private void drainOutput() {
        Threads.raisePriority(Process.THREAD_PRIORITY_URGENT_DISPLAY);
        final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        boolean announced = false;
        try {
            while (running) {
                int index = codec.dequeueOutputBuffer(info, 50_000);
                if (index < 0) continue;

                // If several pictures are ready only the newest is worth showing.
                int dropped = 0;
                long frame = info.presentationTimeUs;
                int newer;
                while ((newer = codec.dequeueOutputBuffer(info, 0)) >= 0) {
                    codec.releaseOutputBuffer(index, false);
                    index = newer;
                    frame = info.presentationTimeUs;
                    dropped++;
                }

                final long now = System.nanoTime();
                final int slot = (int) (frame % TIMING_SLOTS);
                // A presentation time of "now" tells the compositor to replace, not queue behind,
                // any frame still waiting for this vsync.
                codec.releaseOutputBuffer(index, now);

                final int decodeUs = (int) ((now - queuedAtNs[slot]) / 1000);
                if (decodeUs > SLOW_NS / 1000) Log.w(TAG, "decoder took " + decodeUs / 1000 + " ms over a frame");
                outbox.putAck((int) frame, decodeUs, dropped);
                if (!announced) {
                    announced = true;
                    listener.onFirstFrame();
                }
            }
        } catch (IllegalStateException e) {
            if (running) {
                Log.e(TAG, "decoder failed", e);
                listener.onDecoderFailed();
            }
        }
    }
}
