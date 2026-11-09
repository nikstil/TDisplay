package com.tdisplay;

import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Process;
import android.util.Log;
import android.view.Surface;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;

/**
 * Draws each decoded picture onto the screen's surface with the GPU. This MediaTek display hardware can't show
 * the decoder's YUV pictures itself: they go through its MML processor, which puts the whole screen on a
 * schedule about 15 ms longer. An RGB surface goes straight to the panel on the normal schedule.
 */
final class GpuPresenter implements SurfaceTexture.OnFrameAvailableListener {
    private static final String TAG = "TDisplay";
    private static final String VERTEX_SHADER =
            "attribute vec2 corner;\n"
            + "uniform mat4 textureMatrix;\n"
            + "varying highp vec2 uv;\n"
            + "void main() {\n"
            + "    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
            + "    uv = (textureMatrix * vec4(corner, 0.0, 1.0)).xy;\n"
            + "}\n";
    private static final String FRAGMENT_SHADER =
            "#extension GL_OES_EGL_image_external : require\n"
            + "precision highp float;\n"
            + "uniform samplerExternalOES picture;\n"
            + "varying highp vec2 uv;\n"
            + "void main() {\n"
            + "    gl_FragColor = texture2D(picture, uv);\n"
            + "}\n";
    private static final float[] CORNERS = {0, 0, 1, 0, 0, 1, 1, 1};
    private static final long SLOW_NS = 250_000_000L;
    /** How long the tablet keeps itself ready after the pen was last near or the last picture arrived. */
    private static final long READY_NS = 3_000_000_000L;
    /**
     * With nothing reaching the display for about 50 ms, this tablet's memory and processors drop into a deep
     * idle, and the first frames after that can take 30 ms longer to get through. Redrawing a little more often
     * than that keeps it out, at a cost of about 1 W, so it only happens while the tablet is in use.
     */
    private static final int READY_REDRAW_MS = 40;

    private static volatile long readyUntilNs;

    private final Surface output;
    private final int width;
    private final int height;
    private final Runnable onFailure;
    private final Semaphore pictures = new Semaphore(0);
    private final CountDownLatch started = new CountDownLatch(1);

    private Thread thread;
    private volatile boolean running;
    private volatile Surface input;

    GpuPresenter(Surface output, int width, int height, Runnable onFailure) {
        this.output = output;
        this.width = width;
        this.height = height;
        this.onFailure = onFailure;
    }

    /** Someone is using the tablet, so the next frame should get through as fast as it can. */
    static void stayReady() {
        readyUntilNs = System.nanoTime() + READY_NS;
    }

    /** Returns the surface the decoder should send its pictures to. */
    Surface start() throws IOException {
        running = true;
        thread = new Thread(this::run, "tdisplay-gpu");
        thread.start();
        try {
            if (!started.await(2, TimeUnit.SECONDS)) throw new IOException("the GPU took too long to start");
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IOException("interrupted", e);
        }
        if (input == null) throw new IOException("the GPU would not start");
        return input;
    }

    /** Lets go of the screen's surface. The decoder has to have stopped first. */
    void stop() {
        running = false;
        pictures.release();
        if (thread != null) {
            try {
                thread.join(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            thread = null;
        }
    }

    /** Arrives on whichever thread the decoder released the picture from. */
    @Override
    public void onFrameAvailable(SurfaceTexture texture) {
        pictures.release();
    }

    private static int compile(int type, String source) {
        final int shader = GLES20.glCreateShader(type);
        GLES20.glShaderSource(shader, source);
        GLES20.glCompileShader(shader);
        final int[] ok = new int[1];
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, ok, 0);
        if (ok[0] == 0) throw new IllegalStateException("shader: " + GLES20.glGetShaderInfoLog(shader));
        return shader;
    }

    private static int buildProgram() {
        final int program = GLES20.glCreateProgram();
        GLES20.glAttachShader(program, compile(GLES20.GL_VERTEX_SHADER, VERTEX_SHADER));
        GLES20.glAttachShader(program, compile(GLES20.GL_FRAGMENT_SHADER, FRAGMENT_SHADER));
        GLES20.glLinkProgram(program);
        final int[] ok = new int[1];
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, ok, 0);
        if (ok[0] == 0) throw new IllegalStateException("program: " + GLES20.glGetProgramInfoLog(program));
        return program;
    }

    private void run() {
        Threads.raisePriority(Process.THREAD_PRIORITY_URGENT_DISPLAY);
        EGLDisplay display = EGL14.EGL_NO_DISPLAY;
        EGLContext context = EGL14.EGL_NO_CONTEXT;
        EGLSurface window = EGL14.EGL_NO_SURFACE;
        SurfaceTexture texture = null;
        boolean failed = false;
        try {
            display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            final int[] version = new int[2];
            if (!EGL14.eglInitialize(display, version, 0, version, 1)) throw new IllegalStateException("eglInitialize " + EGL14.eglGetError());
            final int[] wanted = {
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_NONE,
            };
            final EGLConfig[] configs = new EGLConfig[1];
            final int[] count = new int[1];
            if (!EGL14.eglChooseConfig(display, wanted, 0, configs, 0, 1, count, 0) || count[0] == 0) throw new IllegalStateException("no EGL config");
            context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT, new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            window = EGL14.eglCreateWindowSurface(display, configs[0], output, new int[] {EGL14.EGL_NONE}, 0);
            if (window == EGL14.EGL_NO_SURFACE || !EGL14.eglMakeCurrent(display, window, window, context)) {
                throw new IllegalStateException("EGL surface " + EGL14.eglGetError());
            }
            // Never wait for the display here: a newer picture simply replaces one that hasn't been shown.
            EGL14.eglSwapInterval(display, 0);

            final int[] textures = new int[1];
            GLES20.glGenTextures(1, textures, 0);
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textures[0]);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glDisable(GLES20.GL_DITHER);
            texture = new SurfaceTexture(textures[0]);
            texture.setDefaultBufferSize(width, height);
            texture.setOnFrameAvailableListener(this, null);

            final int program = buildProgram();
            GLES20.glUseProgram(program);
            final FloatBuffer corners = ByteBuffer.allocateDirect(CORNERS.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
            corners.put(CORNERS).position(0);
            final int corner = GLES20.glGetAttribLocation(program, "corner");
            GLES20.glEnableVertexAttribArray(corner);
            GLES20.glVertexAttribPointer(corner, 2, GLES20.GL_FLOAT, false, 0, corners);
            final int textureMatrix = GLES20.glGetUniformLocation(program, "textureMatrix");
            final float[] matrix = new float[16];
            final int[] size = new int[2];

            input = new Surface(texture);
            started.countDown();

            while (running) {
                final boolean fresh = pictures.tryAcquire(READY_REDRAW_MS, TimeUnit.MILLISECONDS);
                if (!running) continue;
                if (fresh) {
                    stayReady();
                } else if (System.nanoTime() - readyUntilNs > 0) {
                    continue;
                }
                // Pictures that arrived during the last draw are already out of date, so only the newest is drawn.
                final long began = System.nanoTime();
                final int waiting = pictures.drainPermits();
                for (int i = 0; i <= waiting; i++) texture.updateTexImage();
                texture.getTransformMatrix(matrix);
                EGL14.eglQuerySurface(display, window, EGL14.EGL_WIDTH, size, 0);
                EGL14.eglQuerySurface(display, window, EGL14.EGL_HEIGHT, size, 1);
                GLES20.glViewport(0, 0, size[0], size[1]);
                GLES20.glUniformMatrix4fv(textureMatrix, 1, false, matrix, 0);
                GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
                // Keeps the moment the decoder finished as the frame's time, for the compositor's own records.
                EGLExt.eglPresentationTimeANDROID(display, window, fresh ? texture.getTimestamp() : System.nanoTime());
                if (!EGL14.eglSwapBuffers(display, window)) throw new IllegalStateException("eglSwapBuffers " + EGL14.eglGetError());
                final long took = System.nanoTime() - began;
                if (took > SLOW_NS) Log.w(TAG, "drawing a picture took " + took / 1_000_000 + " ms");
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (RuntimeException e) {
            if (running) {
                failed = true;
                Log.e(TAG, "GPU presenter failed", e);
            }
        } finally {
            if (input != null) input.release();
            if (texture != null) texture.release();
            if (display != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                if (window != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, window);
                if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context);
                EGL14.eglReleaseThread();
            }
            if (started.getCount() > 0) {
                input = null;
                started.countDown();
            } else if (failed) {
                onFailure.run();
            }
        }
    }
}
