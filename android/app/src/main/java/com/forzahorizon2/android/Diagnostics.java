package com.forzahorizon2.android;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;
import android.os.Process;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.TimeUnit;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Own-app reports only. Generating a report never uploads or sends it. */
final class Diagnostics {
    private static final int LIMIT = 256 * 1024;
    private static JSONObject cachedGpu;
    // hooks, temp: the adrenotools hook folder and a temporary folder; directory, library: the driver ("" = system).
    private static native String nativeGpuReport(String hooks, String temp, String directory, String library);

    /**
     * The Vulkan test with a driver (null = the system's): opens it through the game's loader, creates an instance
     * and a logical device and lists what the renderers need and the driver lacks. An imported driver only in
     * GpuProbeActivity's throwaway process: adrenotools' hooks stay in the process that opened it.
     */
    static String probe(Context context, GpuDrivers.Driver driver) {
        System.loadLibrary("fh2_diagnostics");
        return nativeGpuReport(GpuDrivers.hookFolder(context).getAbsolutePath(), context.getCacheDir().getAbsolutePath(),
                driver == null ? "" : driver.folder.getAbsolutePath(), driver == null ? "" : driver.library);
    }

    static synchronized JSONObject gpu() {
        if (cachedGpu == null) {
            try {
                System.loadLibrary("fh2_diagnostics");
                cachedGpu = new JSONObject(nativeGpuReport("", "", "", ""));
            } catch (Exception | LinkageError error) {
                // A failed probe is not proof of incompatibility. Record it and
                // let the native runtime perform its normal capability checks.
                cachedGpu = new JSONObject();
                try { cachedGpu.put("probeError", error.toString()); } catch (Exception ignored) { }
            }
        }
        return cachedGpu;
    }

    static String summary(Context context) {
        String version = "desconhecida";
        try { version = context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName; }
        catch (Exception ignored) { }
        JSONObject device = gpu();
        return "FH2 Recompiled " + version + "\nCelular: " + Build.MANUFACTURER + " " + Build.MODEL
                + "\nAndroid: " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")"
                + "\nGPU: " + device.optString("gpu", "indisponível")
                + "\nVulkan: " + device.optString("vulkan", "indisponível")
                + "\nEdição do jogo: " + editionLine(context) + "\n";
    }

    private static String editionLine(Context context) {
        GameEdition edition = GameEdition.of(context,
                new File(android.os.Environment.getExternalStorageDirectory(), MainActivity.GAME_FOLDER_NAME));
        if (edition == null) return "sem default.xex";
        return edition.supported() ? GameEdition.SUPPORTED_NAME : "incompatível (SHA-256 " + edition.sha256.substring(0, 12) + ")";
    }

    // The native renderer's fh2_shaders.nfsp, where the launcher looks for it: path, size and date, so a report
    // tells a missing, partial (cut copy) or old library apart.
    private static String shaderLibraryLine(Context context) {
        File library = MainActivity.shaderLibrary(context);
        if (!library.isFile()) return "não encontrada (" + library.getPath() + ")";
        long bytes = library.length();
        return library.getPath() + " - " + bytes + " bytes (" + String.format(Locale.US, "%.1f", bytes / 1048576.0)
                + " MB), modificada em " + new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US)
                .format(new Date(library.lastModified()));
    }

    static void recordLaunch(Context context) {
        context.getSharedPreferences("fh2_diagnostics", Context.MODE_PRIVATE).edit()
                .putLong("last_launch_ms", System.currentTimeMillis()).putInt("last_launch_pid", Process.myPid()).apply();
    }

    static File createReport(Context context) throws Exception {
        File directory = new File(context.getCacheDir(), "reports");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Não foi possível criar o relatório.");
        // Leave recent attachments available while a mail app or document picker
        // still reads them. Remove only reports older than seven days.
        File[] old = directory.listFiles();
        if (old != null) for (File file : old) {
            if (file.isFile() && file.lastModified() < System.currentTimeMillis() - TimeUnit.DAYS.toMillis(7)) file.delete();
        }
        String date = new SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).format(new Date());
        File report = new File(directory, "FH2-diagnostico-" + date + ".zip");
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(report))) {
            ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
            ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
            if (manager != null) manager.getMemoryInfo(memory);
            StringBuilder info = new StringBuilder(summary(context));
            info.append("Data: ").append(new Date()).append("\nABI: ").append(Arrays.toString(Build.SUPPORTED_ABIS))
                    .append("\nRAM total: ").append(memory.totalMem).append("\nRAM disponível: ").append(memory.availMem)
                    .append("\nÚltima tentativa de iniciar: ").append(context.getSharedPreferences("fh2_diagnostics", 0)
                            .getLong("last_launch_ms", 0))
                    .append("\nBiblioteca de shaders: ").append(shaderLibraryLine(context)).append("\n\nOpções:\n");
            for (java.util.Map.Entry<String, ?> option : new java.util.TreeMap<>(GameOptions.prefs(context).getAll()).entrySet()) {
                info.append(option.getKey()).append('=').append(option.getValue()).append('\n');
            }
            info.append("\nArgumentos efetivos:\n");
            for (String argument : GameOptions.arguments(context)) info.append(argument).append('\n');
            info.append("\nDescreva o que aconteceu e como reproduzir.\n");
            add(zip, "relatorio.txt", info.toString().getBytes(StandardCharsets.UTF_8));
            add(zip, "gpu.json", gpu().toString(2).getBytes(StandardCharsets.UTF_8));
            addFile(zip, "last-java-crash.txt", new File(context.getFilesDir(), "last-java-crash.txt"));
            File logs = new File(context.getFilesDir(), "fh2/user/logs");
            File[] nativeLogs = logs.listFiles(file -> file.isFile() && file.getName().endsWith(".log"));
            if (nativeLogs != null) {
                Arrays.sort(nativeLogs, Comparator.comparingLong(File::lastModified).reversed());
                for (int i = 0; i < Math.min(2, nativeLogs.length); i++) addFile(zip, "native/" + nativeLogs[i].getName(), nativeLogs[i]);
            }
            // The play sessions' own logs (GameActivity.sessionLogBase): the game log and the usage log of the
            // newest two, recut at their start and end like the others.
            for (File session : newestSessions(2)) {
                addFile(zip, "sessao/" + session.getName(), session);
                File usage = new File(session.getParentFile(), session.getName().replace(".log", "_uso.log"));
                addFile(zip, "sessao/" + usage.getName(), usage);
            }
            addLogcat(zip);
            if (Build.VERSION.SDK_INT >= 30 && manager != null) addExitInfo(zip, manager, context);
        } catch (Exception error) {
            report.delete();
            throw error;
        }
        return report;
    }

    // Internal storage/jogos/fh2-logs, where GameActivity writes one game log per session (fh2_<date>_<time>.log,
    // next to its _uso.log). Newest first.
    static File sessionLogFolder() {
        return new File(android.os.Environment.getExternalStorageDirectory(), "jogos/fh2-logs");
    }

    static File[] newestSessions(int count) {
        File[] logs = sessionLogFolder().listFiles(file -> file.isFile() && file.getName().startsWith("fh2_")
                && file.getName().endsWith(".log") && !file.getName().endsWith("_uso.log"));
        if (logs == null) return new File[0];
        Arrays.sort(logs, Comparator.comparingLong(File::lastModified).reversed());
        return Arrays.copyOf(logs, Math.min(count, logs.length));
    }

    private static void add(ZipOutputStream zip, String name, byte[] data) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(data);
        zip.closeEntry();
    }

    private static void addFile(ZipOutputStream zip, String name, File file) throws IOException {
        if (!file.isFile()) return;
        // Keep startup and the last errors, even after a log storm. Never load
        // a whole multi-megabyte native log or attach game data / saved games.
        try (RandomAccessFile input = new RandomAccessFile(file, "r"); ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            long size = input.length();
            int head = (int) Math.min(size, LIMIT / 2);
            byte[] bytes = new byte[head];
            input.readFully(bytes);
            output.write(bytes);
            if (size > head) {
                int tail = (int) Math.min(size - head, LIMIT / 2);
                if (size > LIMIT) output.write("\n[... registro recortado: inicio y final ...]\n".getBytes(StandardCharsets.UTF_8));
                input.seek(size - tail);
                bytes = new byte[tail];
                input.readFully(bytes);
                output.write(bytes);
            }
            add(zip, name, output.toByteArray());
        }
    }

    private static byte[] readLimited(InputStream input) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        while (output.size() < LIMIT) {
            int count = input.read(buffer, 0, Math.min(buffer.length, LIMIT - output.size()));
            if (count < 0) break;
            output.write(buffer, 0, count);
        }
        return output.toByteArray();
    }

    private static void addLogcat(ZipOutputStream zip) throws IOException {
        java.lang.Process process = null;
        try {
            process = new ProcessBuilder("logcat", "-d", "-b", "main", "-b", "crash", "-v", "threadtime",
                    "-t", "1500", "--uid=" + Process.myUid()).redirectErrorStream(true).start();
            java.lang.Process active = process;
            Thread timeout = new Thread(() -> {
                try { if (!active.waitFor(3, TimeUnit.SECONDS)) active.destroyForcibly(); }
                catch (InterruptedException ignored) { active.destroyForcibly(); Thread.currentThread().interrupt(); }
            }, "ReportLogcatTimeout");
            timeout.setDaemon(true);
            timeout.start();
            try (InputStream input = process.getInputStream()) { add(zip, "logcat.txt", readLimited(input)); }
        } catch (IOException error) {
            add(zip, "logcat-error.txt", error.toString().getBytes(StandardCharsets.UTF_8));
        } finally { if (process != null) process.destroy(); }
    }

    private static void addExitInfo(ZipOutputStream zip, ActivityManager manager, Context context) throws IOException {
        try {
            List<ApplicationExitInfo> exits = manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 5);
            StringBuilder info = new StringBuilder();
            boolean traceAdded = false;
            for (ApplicationExitInfo exit : exits) {
                info.append("Timestamp: ").append(exit.getTimestamp()).append(" reason: ").append(exit.getReason())
                        .append(" status: ").append(exit.getStatus()).append(" description: ").append(exit.getDescription()).append('\n');
                if (!traceAdded && (exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE
                        || exit.getReason() == ApplicationExitInfo.REASON_ANR)) {
                    try (InputStream trace = exit.getTraceInputStream()) {
                        if (trace != null) {
                            add(zip, exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE ? "native-crash.pb" : "anr-trace.txt", readLimited(trace));
                            traceAdded = true;
                        }
                    } catch (IOException error) { info.append("Trace unavailable: ").append(error).append('\n'); }
                }
            }
            add(zip, "process-exits.txt", info.toString().getBytes(StandardCharsets.UTF_8));
        } catch (RuntimeException error) {
            Log.w("FH2", "Histórico de encerramentos indisponível", error);
            add(zip, "process-exits.txt", error.toString().getBytes(StandardCharsets.UTF_8));
        }
    }
}
