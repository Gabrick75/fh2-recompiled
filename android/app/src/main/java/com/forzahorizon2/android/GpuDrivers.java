package com.forzahorizon2.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Build;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import static com.forzahorizon2.android.AppLanguage.tr;

/**
 * The phone's GPU name and the Vulkan drivers (Turnip) the player imports. Adapted from NFSMW-Recompiled-Mobile
 * (Gpu.java, Drivers.java).
 *
 * A driver is the .zip of the adrenotools packages (Mesa Turnip builds for emulators), with a meta.json naming the
 * .so to load: { "name": "Turnip v25.x", "libraryName": "libvulkan_freedreno.so", ... }. It is unpacked to
 * files/gpu_drivers/NAME/ - internal storage: adrenotools loads nothing from /sdcard, and neither should dlopen.
 *
 * The game does not load an imported driver yet (that needs libadrenotools in the native side); the launcher only
 * keeps them and remembers the chosen one.
 */
final class GpuDrivers {
    private static final String TAG = "FH2";

    static final class Driver {
        final String name;
        final File folder;
        final String library;

        Driver(String name, File folder, String library) {
            this.name = name;
            this.folder = folder;
            this.library = library;
        }

        // Turnip (Mesa for Adreno): its library is libvulkan_freedreno.so. The GPU turbo only makes sense with it.
        boolean isTurnip() {
            return "libvulkan_freedreno.so".equals(library) || name.toLowerCase(Locale.ROOT).contains("turnip");
        }
    }

    private static String gpuName;

    private GpuDrivers() {
    }

    // ---- The GPU -------------------------------------------------------------------------------------------

    /**
     * "Adreno 740", "Mali-G57 MC2"...; "" when unknown. Asked through a 1x1 EGL context (GL_RENDERER), without
     * Vulkan or the game, and kept while the system build is the same: an update can change the driver.
     */
    static synchronized String gpuName(Context context) {
        if (gpuName != null) {
            return gpuName;
        }
        SharedPreferences prefs = context.getSharedPreferences("gpu", Context.MODE_PRIVATE);
        if (Build.FINGERPRINT.equals(prefs.getString("build", null))) {
            gpuName = prefs.getString("name", "");
            return gpuName;
        }
        String renderer = askEgl();
        gpuName = renderer == null ? ""
                : renderer.replace("(TM)", "").replace("(tm)", "").replaceAll("\\s+", " ").trim();
        prefs.edit().putString("build", Build.FINGERPRINT).putString("name", gpuName).apply();
        return gpuName;
    }

    static boolean isAdreno(Context context) {
        return gpuName(context).toLowerCase(Locale.ROOT).contains("adreno");
    }

    private static String askEgl() {
        EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (display == EGL14.EGL_NO_DISPLAY) {
            return null;
        }
        int[] version = new int[2];
        if (!EGL14.eglInitialize(display, version, 0, version, 1)) {
            return null;
        }
        EGLContext eglContext = EGL14.EGL_NO_CONTEXT;
        EGLSurface surface = EGL14.EGL_NO_SURFACE;
        try {
            int[] attributes = {
                    EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                    EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                    EGL14.EGL_NONE
            };
            EGLConfig[] config = new EGLConfig[1];
            int[] count = new int[1];
            if (!EGL14.eglChooseConfig(display, attributes, 0, config, 0, 1, count, 0) || count[0] == 0) {
                return null;
            }
            eglContext = EGL14.eglCreateContext(display, config[0], EGL14.EGL_NO_CONTEXT,
                    new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            surface = EGL14.eglCreatePbufferSurface(display, config[0],
                    new int[] {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
            if (eglContext == EGL14.EGL_NO_CONTEXT || surface == EGL14.EGL_NO_SURFACE
                    || !EGL14.eglMakeCurrent(display, surface, surface, eglContext)) {
                return null;
            }
            return GLES20.glGetString(GLES20.GL_RENDERER);
        } catch (RuntimeException e) {
            Log.w(TAG, "GPU name", e);
            return null;
        } finally {
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (surface != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(display, surface);
            }
            if (eglContext != EGL14.EGL_NO_CONTEXT) {
                EGL14.eglDestroyContext(display, eglContext);
            }
            // No eglTerminate: the default EGL display is the one Android's UI renderer uses in this process.
        }
    }

    // ---- Imported drivers ----------------------------------------------------------------------------------

    static File root(Context context) {
        File folder = new File(context.getFilesDir(), "gpu_drivers");
        //noinspection ResultOfMethodCallIgnored
        folder.mkdirs();
        return folder;
    }

    static List<Driver> list(Context context) {
        List<Driver> drivers = new ArrayList<>();
        File[] folders = root(context).listFiles(File::isDirectory);
        if (folders == null) {
            return drivers;
        }
        for (File folder : folders) {
            File libraryFile = new File(folder, ".library");
            if (!libraryFile.isFile()) {
                continue;
            }
            try (InputStream in = new FileInputStream(libraryFile)) {
                String library = readText(in).trim();
                if (new File(folder, library).isFile()) {
                    drivers.add(new Driver(folder.getName(), folder, library));
                }
            } catch (IOException ignored) {
                // A half-written folder is not listed.
            }
        }
        return drivers;
    }

    static Driver find(Context context, String name) {
        if (name == null) {
            return null;
        }
        for (Driver driver : list(context)) {
            if (driver.name.equals(name)) {
                return driver;
            }
        }
        return null;
    }

    /** Imports a driver .zip. Throws with a message for the player (shown as is). */
    static Driver importZip(Context context, Uri zip) throws IOException {
        File staging = new File(root(context), ".importing");
        delete(staging);
        if (!staging.mkdirs()) {
            throw new IOException(tr("não foi possível criar ", "could not create ") + staging);
        }
        String metaName = null;
        String metaLibrary = null;
        String firstSo = null;
        try (InputStream in = context.getContentResolver().openInputStream(zip);
             ZipInputStream entries = new ZipInputStream(in)) {
            ZipEntry entry;
            byte[] buffer = new byte[1 << 16];
            while ((entry = entries.getNextEntry()) != null) {
                if (entry.isDirectory()) {
                    continue;
                }
                // Flat, by file name: no paths from the zip, so a "../../x" cannot write outside the folder.
                String name = new File(entry.getName()).getName();
                if (name.isEmpty() || name.startsWith(".")) {
                    continue;
                }
                if (name.equals("meta.json")) {
                    JSONObject meta = new JSONObject(readText(entries));
                    metaName = meta.optString("name", null);
                    metaLibrary = meta.optString("libraryName", null);
                    continue;
                }
                if (name.endsWith(".so") && firstSo == null) {
                    firstSo = name;
                }
                try (FileOutputStream out = new FileOutputStream(new File(staging, name))) {
                    for (int count; (count = entries.read(buffer)) > 0; ) {
                        out.write(buffer, 0, count);
                    }
                }
            }
        } catch (JSONException e) {
            delete(staging);
            throw new IOException(tr("o meta.json não é um JSON válido", "meta.json is not valid JSON"));
        } catch (IOException e) {
            delete(staging);
            throw e;
        }
        String library = metaLibrary != null ? metaLibrary : firstSo;
        if (library == null || !new File(staging, library).isFile()) {
            delete(staging);
            throw new IOException(tr("o zip não tem nenhuma .so de driver", "the zip has no driver .so"));
        }
        String name = (metaName != null ? metaName : library.replace(".so", ""))
                .replaceAll("[^A-Za-z0-9._-]+", "_");
        if (name.isEmpty()) {
            name = "driver";
        }
        File destination = new File(root(context), name);
        delete(destination);
        if (!staging.renameTo(destination)) {
            delete(staging);
            throw new IOException(tr("não foi possível mover o driver para ", "could not move the driver to ") + destination);
        }
        try (FileOutputStream out = new FileOutputStream(new File(destination, ".library"))) {
            out.write(library.getBytes(StandardCharsets.UTF_8));
        }
        // Read only: Android's loader refuses libraries the app could rewrite while it uses them.
        File[] files = destination.listFiles();
        if (files != null) {
            for (File file : files) {
                //noinspection ResultOfMethodCallIgnored
                file.setWritable(false, false);
            }
        }
        return new Driver(name, destination, library);
    }

    static void remove(Driver driver) {
        delete(driver.folder);
    }

    private static String readText(InputStream in) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        for (int count; (count = in.read(buffer)) > 0; ) {
            bytes.write(buffer, 0, count);
        }
        return bytes.toString("UTF-8");
    }

    private static void delete(File file) {
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    delete(child);
                }
            }
        }
        //noinspection ResultOfMethodCallIgnored
        file.setWritable(true, true);
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }
}
