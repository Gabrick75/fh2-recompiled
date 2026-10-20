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
    /*
     * Per file. It was 256 KB: with the native renderer that is the first ~5 s and the last ~4 s of a session,
     * and usuario-20's black screen fell in the cut middle of four reports in a row. Logs compress about 3.5x
     * in the zip, so 5 MB per session log stays a reasonable attachment.
     */
    private static final int LIMIT = 5 * 1024 * 1024;
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
        return "FH2 Recompiled " + version + " (commit " + BuildConfig.GIT_COMMIT + " de " + BuildConfig.GIT_DATE + ")"
                + "\nCelular: " + Build.MANUFACTURER + " " + Build.MODEL
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

    // The newest play session at a glance, all read from its logs when the report is made (nothing measured
    // during the game): when it ran, how the process ended, the FPS of its usage log, the Vulkan driver the game
    // really opened and its last errors.
    private static String lastSessionSummary(Context context, ActivityManager manager) {
        StringBuilder out = new StringBuilder("Última sessão:\n");
        File[] sessions = newestSessions(1);
        if (sessions.length == 0) return out.append("  nenhuma em ").append(sessionLogFolder()).append('\n').toString();
        File log = sessions[0];
        long start = sessionStart(log);
        out.append("  arquivo: ").append(log.getName()).append('\n');
        out.append("  início: ").append(start > 0 ? formatTime(start) : "desconhecido")
                .append(", último registro: ").append(formatTime(log.lastModified()));
        if (start > 0 && log.lastModified() >= start) {
            out.append(" (").append((log.lastModified() - start) / 1000).append(" s)");
        }
        out.append('\n').append("  término: ").append(exitLine(context, manager, start)).append('\n');
        out.append("  FPS: ").append(fpsLine(new File(log.getParentFile(), log.getName().replace(".log", "_uso.log"))))
                .append('\n');
        String requested = null, device = null;
        java.util.ArrayDeque<String> errors = new java.util.ArrayDeque<>();
        try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.InputStreamReader(
                new java.io.FileInputStream(log), StandardCharsets.UTF_8))) {
            for (String line; (line = reader.readLine()) != null; ) {
                if (requested == null && line.contains("Vulkan requested driver:")) {
                    requested = afterLevel(line).replace("Vulkan requested driver:", "").trim();
                }
                if (device == null && line.contains("Vulkan device '")) device = afterLevel(line);
                if ((line.contains("[error]") || line.contains("[critical]") || line.contains("FATAL"))
                        && !IGNORED_ERROR.matcher(line).find()) {
                    if (errors.size() == 5) errors.removeFirst();
                    errors.addLast(line.length() > 300 ? line.substring(0, 300) + "…" : line);
                }
            }
        } catch (IOException error) {
            out.append("  (log ilegível: ").append(error.getMessage()).append(")\n");
        }
        out.append("  driver pedido: ").append(requested != null ? requested : "o do sistema").append('\n');
        out.append("  driver aberto: ").append(device != null ? device : "não registrado").append('\n');
        out.append("  último crash Java: ").append(javaCrashLine(context, start)).append('\n');
        out.append("  últimos erros:").append(errors.isEmpty() ? " nenhum\n" : "\n");
        for (String error : errors) out.append("    ").append(error).append('\n');
        return out.toString();
    }

    // What the phone and the game files look like when the report is made: storage, thermal and battery state,
    // display rates, the connected pads and the phone's vibrator. Each part fails on its own.
    private static String deviceAndGame(Context context) {
        StringBuilder out = new StringBuilder("Aparelho e jogo:\n");
        try {
            File iso = GameIso.chosen(context);
            if (iso != null) {
                out.append("  jogo: ISO ").append(iso.getPath()).append(" (")
                        .append(String.format(Locale.US, "%.2f", iso.length() / 1073741824.0)).append(" GB)\n");
            } else {
                File folder = MainActivity.gameRoot();
                out.append("  jogo: pasta ").append(folder.getPath())
                        .append(new File(folder, "default.xex").isFile() ? " (default.xex presente)" : " (SEM default.xex)")
                        .append('\n');
            }
            android.os.StatFs storage = new android.os.StatFs(android.os.Environment.getExternalStorageDirectory().getPath());
            out.append("  armazenamento livre: ")
                    .append(String.format(Locale.US, "%.1f", storage.getAvailableBytes() / 1073741824.0))
                    .append(" GB de ").append(String.format(Locale.US, "%.1f", storage.getTotalBytes() / 1073741824.0))
                    .append(" GB\n");
            long[] user = folderSize(SaveTransfer.userRoot(context));
            out.append("  dados do usuário (saves): ").append(user[1]).append(" arquivos, ")
                    .append(String.format(Locale.US, "%.1f", user[0] / 1048576.0)).append(" MB\n");
        } catch (RuntimeException error) {
            out.append("  arquivos: indisponível (").append(error.getMessage()).append(")\n");
        }
        try {
            android.os.PowerManager power = (android.os.PowerManager) context.getSystemService(Context.POWER_SERVICE);
            if (power != null) {
                if (Build.VERSION.SDK_INT >= 29) {
                    out.append("  estado térmico: ").append(thermalName(power.getCurrentThermalStatus())).append('\n');
                }
                out.append("  economia de energia: ").append(power.isPowerSaveMode() ? "LIGADA" : "desligada").append('\n');
            }
            android.content.Intent battery = context.registerReceiver(null,
                    new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED));
            if (battery != null) {
                int level = battery.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1);
                int scale = battery.getIntExtra(android.os.BatteryManager.EXTRA_SCALE, 100);
                int tenths = battery.getIntExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, 0);
                int plugged = battery.getIntExtra(android.os.BatteryManager.EXTRA_PLUGGED, 0);
                out.append("  bateria: ").append(scale > 0 ? level * 100 / scale : level).append(" %, ")
                        .append(tenths / 10.0).append(" °C").append(plugged != 0 ? ", carregando" : "").append('\n');
            }
            android.hardware.display.DisplayManager displays =
                    (android.hardware.display.DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
            android.view.Display display = displays != null ? displays.getDisplay(android.view.Display.DEFAULT_DISPLAY) : null;
            if (display != null) {
                java.util.TreeSet<Integer> rates = new java.util.TreeSet<>();
                for (android.view.Display.Mode mode : display.getSupportedModes()) rates.add(Math.round(mode.getRefreshRate()));
                out.append("  tela: ").append(Math.round(display.getRefreshRate())).append(" Hz agora, suporta ")
                        .append(rates).append(" Hz\n");
            }
        } catch (RuntimeException error) {
            out.append("  estado do aparelho: indisponível (").append(error.getMessage()).append(")\n");
        }
        try {
            android.hardware.input.InputManager input =
                    (android.hardware.input.InputManager) context.getSystemService(Context.INPUT_SERVICE);
            int pads = 0;
            if (input != null) {
                for (int id : input.getInputDeviceIds()) {
                    android.view.InputDevice device = input.getInputDevice(id);
                    if (device == null || device.isVirtual()) continue;
                    int sources = device.getSources();
                    if ((sources & android.view.InputDevice.SOURCE_GAMEPAD) != android.view.InputDevice.SOURCE_GAMEPAD
                            && (sources & android.view.InputDevice.SOURCE_JOYSTICK) != android.view.InputDevice.SOURCE_JOYSTICK) {
                        continue;
                    }
                    ++pads;
                    out.append("  controle: ").append(device.getName())
                            .append(String.format(Locale.US, " (vendor %04x, product %04x)", device.getVendorId(),
                                    device.getProductId()))
                            .append(device.getVibrator().hasVibrator() ? ", vibra" : ", sem vibração").append('\n');
                }
            }
            if (pads == 0) out.append("  controle: nenhum físico conectado (toque)\n");
            out.append("  arquivos de uso da GPU (o primeiro com valor é o da tela):\n")
                    .append(UsageMonitor.gpuProbeReport());
            android.os.Vibrator vibrator = (android.os.Vibrator) context.getSystemService(Context.VIBRATOR_SERVICE);
            out.append("  vibração do celular: ").append(vibrator == null || !vibrator.hasVibrator() ? "não tem"
                    : vibrator.hasAmplitudeControl() ? "controla a intensidade" : "só liga e desliga (sem intensidade)")
                    .append('\n');
            // Silent mode and Do Not Disturb are what most often mute an app's vibration.
            android.media.AudioManager audio = (android.media.AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
            if (audio != null) {
                int ringer = audio.getRingerMode();
                out.append("  modo do toque: ").append(ringer == android.media.AudioManager.RINGER_MODE_SILENT ? "silencioso"
                        : ringer == android.media.AudioManager.RINGER_MODE_VIBRATE ? "só vibrar" : "som").append('\n');
            }
            android.app.NotificationManager notifications =
                    (android.app.NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
            if (notifications != null) {
                int filter = notifications.getCurrentInterruptionFilter();
                out.append("  não perturbe: ").append(filter == android.app.NotificationManager.INTERRUPTION_FILTER_ALL
                        || filter == android.app.NotificationManager.INTERRUPTION_FILTER_UNKNOWN ? "desligado"
                        : "ligado (filtro " + filter + ")").append('\n');
            }
        } catch (RuntimeException error) {
            out.append("  controles: indisponível (").append(error.getMessage()).append(")\n");
        }
        return out.toString();
    }

    // {bytes, files} under a folder (the saves: small, a few hundred files at most).
    private static long[] folderSize(File folder) {
        long[] total = {0, 0};
        File[] children = folder.listFiles();
        if (children == null) return total;
        for (File child : children) {
            if (child.isDirectory()) {
                long[] sub = folderSize(child);
                total[0] += sub[0];
                total[1] += sub[1];
            } else {
                total[0] += child.length();
                ++total[1];
            }
        }
        return total;
    }

    private static String thermalName(int status) {
        switch (status) {
            case android.os.PowerManager.THERMAL_STATUS_NONE: return "normal";
            case android.os.PowerManager.THERMAL_STATUS_LIGHT: return "leve";
            case android.os.PowerManager.THERMAL_STATUS_MODERATE: return "moderado";
            case android.os.PowerManager.THERMAL_STATUS_SEVERE: return "SEVERO (o sistema corta o desempenho)";
            case android.os.PowerManager.THERMAL_STATUS_CRITICAL: return "CRÍTICO";
            case android.os.PowerManager.THERMAL_STATUS_EMERGENCY: return "EMERGÊNCIA";
            case android.os.PowerManager.THERMAL_STATUS_SHUTDOWN: return "DESLIGANDO";
            default: return "desconhecido (" + status + ")";
        }
    }

    // last-java-crash.txt (attached to every report) with its date, flagged when it comes from before the session:
    // an old crash otherwise reads like the cause of a new problem.
    private static String javaCrashLine(Context context, long start) {
        File crash = new File(context.getFilesDir(), "last-java-crash.txt");
        if (!crash.isFile()) return "nenhum";
        long when = crash.lastModified();
        try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.InputStreamReader(
                new java.io.FileInputStream(crash), StandardCharsets.UTF_8))) {
            String first = reader.readLine();
            if (first != null && first.startsWith("Timestamp: ")) when = Long.parseLong(first.substring(11).trim());
        } catch (IOException | NumberFormatException ignored) {
            // The file's date, then.
        }
        return formatTime(when) + (start > 0 && when < start ? " (ANTES desta sessão: antigo)" : " (nesta sessão)")
                + ", em last-java-crash.txt";
    }

    // The same noise tools/ler_partida.sh leaves out.
    private static final java.util.regex.Pattern IGNORED_ERROR = java.util.regex.Pattern.compile(
            "renderdoc|ResolvePath|XMsgStartIORequestEx|Unimplemented XAM", java.util.regex.Pattern.CASE_INSENSITIVE);

    // fh2_<yyyyMMdd>_<HHmmss>.log, GameActivity.sessionLogBase.
    private static long sessionStart(File log) {
        try {
            String name = log.getName();
            Date date = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).parse(name.substring(4, 19));
            return date != null ? date.getTime() : 0;
        } catch (Exception error) {
            return 0;
        }
    }

    private static String formatTime(long ms) {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date(ms));
    }

    // "[2026-10-17 16:57:48.327] [info] [core] [t31771] text" -> "text".
    private static String afterLevel(String line) {
        int at = line.indexOf("] [t");
        int end = at >= 0 ? line.indexOf("] ", at + 3) : -1;
        return end >= 0 ? line.substring(end + 2) : line;
    }

    // The game process's newest exit after the session started (Android 11+).
    private static String exitLine(Context context, ActivityManager manager, long start) {
        if (Build.VERSION.SDK_INT < 30 || manager == null) return "desconhecido (Android 10 ou anterior)";
        try {
            for (ApplicationExitInfo exit : manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 10)) {
                if (!context.getPackageName().equals(exit.getProcessName())) continue;
                if (start > 0 && exit.getTimestamp() < start) break;
                return exitReason(exit) + " em " + formatTime(exit.getTimestamp());
            }
        } catch (RuntimeException error) {
            return "indisponível (" + error.getMessage() + ")";
        }
        return "sem registro (o jogo ainda estava aberto, ou o sistema não registrou)";
    }

    private static String exitReason(ApplicationExitInfo exit) {
        switch (exit.getReason()) {
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "CRASH NATIVO (sinal " + exit.getStatus() + ")";
            case ApplicationExitInfo.REASON_CRASH: return "CRASH JAVA";
            case ApplicationExitInfo.REASON_ANR: return "TRAVOU (ANR)";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "fechado por falta de memória";
            case ApplicationExitInfo.REASON_SIGNALED: return "morto pelo sinal " + exit.getStatus();
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "fechado por uso excessivo de recursos";
            case ApplicationExitInfo.REASON_EXIT_SELF: return "saiu sozinho (código " + exit.getStatus() + ")";
            case ApplicationExitInfo.REASON_USER_REQUESTED: return "fechado pelo usuário";
            case ApplicationExitInfo.REASON_USER_STOPPED: return "parado pelo usuário";
            case ApplicationExitInfo.REASON_PACKAGE_UPDATED: return "fechado por uma atualização do app";
            default: return "motivo " + exit.getReason() + " (" + exit.getDescription() + ")";
        }
    }

    // Usage log lines "HH:MM:SS  27.5 fps  GPU ...", one every 5 s; -1.0 (nothing measured yet) is left out.
    private static String fpsLine(File usage) {
        if (!usage.isFile()) return "sem log de uso";
        double min = Double.MAX_VALUE, max = 0, sum = 0;
        int count = 0;
        java.util.regex.Pattern fps = java.util.regex.Pattern.compile("^\\d\\d:\\d\\d:\\d\\d\\s+(-?[0-9.]+) fps");
        try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.InputStreamReader(
                new java.io.FileInputStream(usage), StandardCharsets.UTF_8))) {
            for (String line; (line = reader.readLine()) != null; ) {
                java.util.regex.Matcher m = fps.matcher(line);
                if (!m.find()) continue;
                double value = Double.parseDouble(m.group(1));
                if (value <= 0) continue;
                min = Math.min(min, value);
                max = Math.max(max, value);
                sum += value;
                ++count;
            }
        } catch (IOException | NumberFormatException error) {
            return "log de uso ilegível";
        }
        if (count == 0) return "nenhuma medida";
        return String.format(Locale.US, "mínimo %.1f, médio %.1f, máximo %.1f (%d medidas de 5 s)",
                min, sum / count, max, count);
    }

    static void recordLaunch(Context context) {
        context.getSharedPreferences("fh2_diagnostics", Context.MODE_PRIVATE).edit()
                .putLong("last_launch_ms", System.currentTimeMillis()).putInt("last_launch_pid", Process.myPid()).apply();
    }

    static File createReport(Context context) throws Exception {
        return createReport(context, "");
    }

    static File createReport(Context context, String description) throws Exception {
        return createReport(context, description, false);
    }

    /**
     * description: what the player wrote before saving it ("" = nothing). root: add the root/ folder
     * (RootDiagnostics.collect; the caller already got the permission with RootDiagnostics.available).
     */
    static File createReport(Context context, String description, boolean root) throws Exception {
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
                    .append("\nBiblioteca de shaders: ").append(shaderLibraryLine(context))
                    .append("\n\n").append(lastSessionSummary(context, manager))
                    .append('\n').append(deviceAndGame(context)).append("\nOpções:\n");
            for (java.util.Map.Entry<String, ?> option : new java.util.TreeMap<>(GameOptions.prefs(context).getAll()).entrySet()) {
                info.append(option.getKey()).append('=').append(option.getValue()).append('\n');
            }
            info.append("\nArgumentos efetivos:\n");
            for (String argument : GameOptions.arguments(context)) info.append(argument).append('\n');
            if (description == null || description.trim().isEmpty()) {
                info.append("\nDescreva o que aconteceu e como reproduzir.\n");
            } else {
                info.append("\nDescrição do jogador:\n").append(description.trim()).append('\n');
            }
            if (root) {
                java.util.Map<String, byte[]> extra = RootDiagnostics.collect();
                info.append("\nColeta com root: ").append(new String(extra.remove(""), StandardCharsets.UTF_8))
                        .append(" (pasta root/ do zip)\n");
                for (java.util.Map.Entry<String, byte[]> file : extra.entrySet()) add(zip, file.getKey(), file.getValue());
            }
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
                File profile = new File(session.getParentFile(), session.getName().replace(".log", "_perfil.txt"));
                addFile(zip, "sessao/" + profile.getName(), profile);
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
        // Keep startup and the last errors, even after a log storm. Never load more
        // than LIMIT of a native log or attach game data / saved games.
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
