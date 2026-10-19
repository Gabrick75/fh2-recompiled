package com.forzahorizon2.android;

import android.content.Context;
import android.graphics.Matrix;
import android.graphics.SurfaceTexture;
import android.util.Log;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;

import java.io.File;

/**
 * The home screen's background: the game's own "press start" video (media/ui/videos/PressStart.wmv), read in place
 * from the disc image or the extracted folder and played by libfh2_home_video (android_home_video.cpp) on a
 * TextureView, in a loop, filling the view (cropped, never letterboxed). Plays only while the activity is in front and
 * the view is on screen; without the game files there is nothing to play and the home screen keeps its grey.
 */
final class HomeVideo implements TextureView.SurfaceTextureListener {
    static final String PATH = "media/ui/videos/PressStart.wmv";
    private static final int WIDTH = 1280, HEIGHT = 720;
    private static boolean loaded;

    private final TextureView view;
    private final View layer;  // faded in with the first frame: the video and its veil
    private long handle;
    private Surface surface;
    private boolean resumed, playing, shown, sound;

    private static native long nativeOpen(String path, long offset, long size);
    private static native void nativeStart(long handle, Surface surface, boolean sound);
    private static native void nativeStop(long handle);
    private static native void nativeSetSound(long handle, boolean sound);
    private static native void nativeClose(long handle);

    /** The view shows the video once a source is set; layer is what fades in with it. */
    HomeVideo(TextureView view, View layer) {
        this.view = view;
        this.layer = layer;
        layer.setAlpha(0f);
        view.setSurfaceTextureListener(this);
    }

    /** Opens the game's video, or forgets the old one when the game is gone. True when there is one to play. */
    boolean setSource(Context context, boolean withSound) {
        close();
        sound = withSound;
        if (!loadLibrary()) return false;
        File root = GameIso.gameDataRoot(context);
        String path;
        long offset, size;
        if (root.isFile()) {
            long[] where = GameIso.find(root, PATH);
            if (where == null) return false;
            path = root.getPath();
            offset = where[0];
            size = where[1];
        } else {
            File file = new File(root, PATH);
            if (!file.isFile()) return false;
            path = file.getPath();
            offset = 0;
            size = file.length();
        }
        handle = nativeOpen(path, offset, size);
        update();
        return handle != 0;
    }

    void setSound(boolean withSound) {
        sound = withSound;
        if (handle != 0) nativeSetSound(handle, withSound);
    }

    void resume() {
        resumed = true;
        update();
    }

    void pause() {
        resumed = false;
        update();
    }

    void close() {
        stop();
        if (handle != 0) nativeClose(handle);
        handle = 0;
        shown = false;
        layer.animate().cancel();
        layer.setAlpha(0f);
    }

    private void update() {
        boolean play = handle != 0 && resumed && surface != null;
        if (play && !playing) {
            nativeStart(handle, surface, sound);
            playing = true;
        } else if (!play) {
            stop();
        }
    }

    private void stop() {
        if (playing) nativeStop(handle);
        playing = false;
    }

    private static boolean loadLibrary() {
        if (!loaded) {
            try {
                System.loadLibrary("fh2_home_video");
                loaded = true;
            } catch (UnsatisfiedLinkError e) {
                Log.w("FH2", "home video: " + e.getMessage());
            }
        }
        return loaded;
    }

    /** Fills the view keeping 16:9: the overflow is cropped evenly on both sides. */
    private void fill(int width, int height) {
        if (width == 0 || height == 0) return;
        float scale = Math.max(width / (float) WIDTH, height / (float) HEIGHT);
        Matrix matrix = new Matrix();
        matrix.setScale(WIDTH * scale / width, HEIGHT * scale / height, width / 2f, height / 2f);
        view.setTransform(matrix);
    }

    @Override
    public void onSurfaceTextureAvailable(SurfaceTexture texture, int width, int height) {
        surface = new Surface(texture);
        fill(width, height);
        update();
    }

    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture texture, int width, int height) {
        fill(width, height);
    }

    @Override
    public boolean onSurfaceTextureDestroyed(SurfaceTexture texture) {
        stop();
        if (surface != null) surface.release();
        surface = null;
        shown = false;
        layer.setAlpha(0f);
        return true;
    }

    @Override
    public void onSurfaceTextureUpdated(SurfaceTexture texture) {
        if (shown || handle == 0) return;
        shown = true;
        layer.animate().alpha(1f).setDuration(600).start();
    }
}
