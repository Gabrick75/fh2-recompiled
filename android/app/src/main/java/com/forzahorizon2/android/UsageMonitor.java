package com.forzahorizon2.android;

import android.app.ActivityManager;
import android.content.Context;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * GPU, CPU and RAM use for the in-game overlay ("Mostrar uso de GPU, CPU e RAM"). Call sample() about once a second,
 * off the UI thread: it reads a few dozen /proc files.
 *
 * - GPU: the kernel driver's busy percentage (Adreno kgsl, Mali on Samsung/Google, MediaTek's GED or the Mali
 *   devfreq). Where none of them can be read (Android 16 on MediaTek blocks them all), the native renderer's own
 *   GPU time per second, from two timestamps per submission (fh2_nativo_medir_gpu_uso); "--" without either.
 * - CPU: the game's share of all cores, and its busiest thread as a share of one core. When that thread is near
 *   100 % (usually "GPU Commands", the emulated GPU's command processor), it is what limits the frame rate.
 * - RAM: the game's resident memory, and the phone's available memory of its total.
 */
final class UsageMonitor {
    private static final String TAG = "fh2";
    // Linux reports CPU times in clock ticks of 1/100 s (USER_HZ) on Android.
    private static final double TICKS_PER_SECOND = 100.0;
    private static final String[] GPU_FILES = {
            "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",  // Adreno: "42 %"
            "/sys/class/kgsl/kgsl-3d0/gpubusy",              // Adreno: "busy total"
            "/sys/class/misc/mali0/device/utilization",      // Mali (some kernels)
            "/sys/kernel/gpu/gpu_busy",                      // Mali (Samsung, Google)
            // MediaTek (Dimensity, Mali): none of the above exist there; its GED module has the load.
            "/sys/kernel/ged/hal/gpu_utilization",           // "loading block idle", e.g. "37 0 63"
            "/sys/module/ged/parameters/gpu_loading",        // "37"
    };

    // GPU clock and its cap (Adreno kgsl, in Hz). The cap is what the system's thermal and power limits leave: on
    // the S23 Ultra it falls from 719 to ~348 MHz within a minute or two of play, and the GPU % then reads 99 %
    // with the same work.
    private static final String[][] GPU_CLOCK_FILES = {
            {"/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", "/sys/class/kgsl/kgsl-3d0/devfreq/max_freq"},
            {"/sys/class/kgsl/kgsl-3d0/gpuclk", "/sys/class/kgsl/kgsl-3d0/max_gpuclk"},
    };

    private final Context context;
    private final ActivityManager activityManager;
    private final int cores = Math.max(1, Runtime.getRuntime().availableProcessors());
    private String gpuFile;
    private boolean gpuLogged;
    private long lastGpuNs = -1;
    private long lastGpuTime;
    private long lastProcessTicks = -1;
    private long lastTime;
    private Map<String, Long> lastThreadTicks = new HashMap<>();
    // Clocks for the usage log, summed every second (sampleClocks) and written as averages every 5 s (clocksLine):
    // the current clock is averaged, the cap is the lowest seen in the window.
    private String[] gpuClockFiles;
    private boolean gpuClocksTried;
    private File[] cpuPolicies;
    private long gpuCurSum, gpuCapMin = Long.MAX_VALUE;
    private long[] cpuCurSum, cpuCapMin;
    private int clockSamples;

    UsageMonitor(Context context) {
        this.context = context.getApplicationContext();
        activityManager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
    }

    /** Adds one sample of the GPU and CPU clocks and their caps; about once a second. */
    void sampleClocks() {
        if (!gpuClocksTried) {
            gpuClocksTried = true;
            for (String[] files : GPU_CLOCK_FILES) {
                if (readLong(files[0]) > 0 && readLong(files[1]) > 0) {
                    gpuClockFiles = files;
                    break;
                }
            }
            File[] policies = new File("/sys/devices/system/cpu/cpufreq").listFiles(
                    f -> f.getName().startsWith("policy"));
            if (policies != null) {
                java.util.Arrays.sort(policies, (a, b) -> Integer.compare(policyNumber(a), policyNumber(b)));
                cpuPolicies = policies;
                cpuCurSum = new long[policies.length];
                cpuCapMin = new long[policies.length];
                java.util.Arrays.fill(cpuCapMin, Long.MAX_VALUE);
            }
        }
        if (gpuClockFiles != null) {
            gpuCurSum += Math.max(0, readLong(gpuClockFiles[0]));
            long cap = readLong(gpuClockFiles[1]);
            if (cap > 0) gpuCapMin = Math.min(gpuCapMin, cap);
        }
        if (cpuPolicies != null) {
            for (int i = 0; i < cpuPolicies.length; i++) {
                cpuCurSum[i] += Math.max(0, readLong(cpuPolicies[i].getPath() + "/scaling_cur_freq"));
                long cap = readLong(cpuPolicies[i].getPath() + "/scaling_max_freq");
                if (cap > 0) cpuCapMin[i] = Math.min(cpuCapMin[i], cap);
            }
        }
        clockSamples++;
    }

    /**
     * The clocks since the previous call, for the usage log: "GPU avg/cap MHz", "CPU avg/cap" per core cluster (in
     * policy order, e.g. small, big, prime), the battery temperature and whether it charges, and Android's thermal
     * headroom (1.0 = throttling; API 30+).
     */
    String clocksLine() {
        StringBuilder line = new StringBuilder();
        int n = Math.max(1, clockSamples);
        if (gpuClockFiles != null && clockSamples > 0) {
            // kgsl reports Hz; some kernels report MHz in gpuclk.
            long scale = gpuCapMin != Long.MAX_VALUE && gpuCapMin > 100000 ? 1000000 : 1;
            line.append(String.format(Locale.ROOT, "  GPU %d/%d MHz", gpuCurSum / n / scale,
                    gpuCapMin == Long.MAX_VALUE ? -1 : gpuCapMin / scale));
        }
        if (cpuPolicies != null && clockSamples > 0) {
            line.append("  CPU");
            for (int i = 0; i < cpuPolicies.length; i++) {
                line.append(String.format(Locale.ROOT, " %d/%d", cpuCurSum[i] / n / 1000,
                        cpuCapMin[i] == Long.MAX_VALUE ? -1 : cpuCapMin[i] / 1000));
                cpuCurSum[i] = 0;
                cpuCapMin[i] = Long.MAX_VALUE;
            }
            line.append(" MHz");
        }
        gpuCurSum = 0;
        gpuCapMin = Long.MAX_VALUE;
        clockSamples = 0;
        android.content.Intent battery = context.registerReceiver(null,
                new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED));
        if (battery != null) {
            int tenths = battery.getIntExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
            boolean plugged = battery.getIntExtra(android.os.BatteryManager.EXTRA_PLUGGED, 0) != 0;
            if (tenths != Integer.MIN_VALUE) {
                line.append(String.format(Locale.ROOT, "  bat %.1f C%s", tenths / 10.0, plugged ? " (carregando)" : ""));
            }
        }
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            android.os.PowerManager power = (android.os.PowerManager) context.getSystemService(Context.POWER_SERVICE);
            if (power != null) {
                float headroom = power.getThermalHeadroom(0);
                if (!Float.isNaN(headroom)) {
                    line.append(String.format(Locale.ROOT, "  termico %.2f", headroom));
                }
            }
        }
        return line.toString();
    }

    private static int policyNumber(File policy) {
        try {
            return Integer.parseInt(policy.getName().substring("policy".length()));
        } catch (NumberFormatException e) {
            return Integer.MAX_VALUE;
        }
    }

    private static long readLong(String path) {
        String line = readLine(path);
        if (line == null) return -1;
        try {
            return Long.parseLong(line.trim().split("\\s+")[0]);
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    /** One line for the overlay. The first call only starts counting the CPU time. */
    String sample() {
        long now = System.nanoTime();
        double seconds = lastProcessTicks < 0 ? 0 : (now - lastTime) / 1e9;
        lastTime = now;

        // CPU: the whole process, then each thread by its name (the busiest one).
        String cpu = "CPU --";
        long processTicks = ticksOf(readLine("/proc/self/stat"));
        if (processTicks >= 0) {
            if (seconds > 0) {
                double percent = 100.0 * (processTicks - lastProcessTicks) / TICKS_PER_SECOND / seconds / cores;
                cpu = String.format(Locale.ROOT, "CPU %.0f%%", percent);
            }
            lastProcessTicks = processTicks;
        }
        Map<String, Long> threadTicks = new HashMap<>();
        String busiest = null;
        double busiestPercent = -1;
        File[] tasks = new File("/proc/self/task").listFiles();
        if (tasks != null) {
            for (File task : tasks) {
                String line = readLine(task.getPath() + "/stat");
                long ticks = ticksOf(line);
                if (ticks < 0) continue;
                // The name alone can repeat (several "Thread-N" are fine to mix up), the id keeps them apart.
                String key = task.getName() + " " + nameOf(line);
                threadTicks.put(key, ticks);
                Long before = lastThreadTicks.get(key);
                if (before != null && seconds > 0) {
                    double percent = 100.0 * (ticks - before) / TICKS_PER_SECOND / seconds;
                    if (percent > busiestPercent) {
                        busiestPercent = percent;
                        busiest = nameOf(line);
                    }
                }
            }
        }
        lastThreadTicks = threadTicks;
        if (busiest != null) {
            cpu += String.format(Locale.ROOT, " (%s %.0f%%)", busiest, Math.min(100, busiestPercent));
        }

        // RAM: resident pages of the process, and the phone's available memory.
        String ram = "RAM --";
        String statm = readLine("/proc/self/statm");
        if (statm != null) {
            String[] fields = statm.split(" ");
            if (fields.length > 1) {
                try {
                    double gameGb = Long.parseLong(fields[1]) * 4096.0 / (1 << 30);
                    ram = String.format(Locale.ROOT, "RAM %.1f GB", gameGb);
                } catch (NumberFormatException ignored) {
                    // keeps "RAM --"
                }
            }
        }
        if (activityManager != null) {
            ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
            activityManager.getMemoryInfo(info);
            ram += String.format(Locale.ROOT, AppLanguage.tr(R.string.free_1f_1f), info.availMem / (double) (1 << 30),
                    info.totalMem / (double) (1 << 30));
        }

        return gpu() + "  " + cpu + "  " + ram;
    }

    private String gpu() {
        if (gpuFile == null) {
            // The first file whose value makes sense, not just the first readable one: a file that reads but is
            // not a percentage would otherwise hold "--" for the whole session.
            for (String path : gpuCandidates()) {
                if (gpuPercent(path, readLine(path)) != null) {
                    gpuFile = path;
                    break;
                }
            }
            if (!gpuLogged) {
                gpuLogged = true;
                Log.i(TAG, "GPU use read from " + (gpuFile != null ? gpuFile : "nowhere (no readable file)"));
            }
            if (gpuFile == null) return timestampGpu();
        }
        Double percent = gpuPercent(gpuFile, readLine(gpuFile));
        return percent == null ? "GPU --" : String.format(Locale.ROOT, "GPU %.0f%%", percent);
    }

    /** The GPU's busy share of the last second from the renderer's timestamps; "GPU --" without them. */
    private String timestampGpu() {
        long ns;
        try {
            ns = nativeGpuNs();
        } catch (UnsatisfiedLinkError e) {
            ns = -1;
        }
        long now = System.nanoTime();
        String text = "GPU --";
        if (ns >= 0 && lastGpuNs >= 0 && now > lastGpuTime) {
            // Consecutive submissions can overlap by a little (the next one starts at the top of the pipe).
            text = String.format(Locale.ROOT, "GPU %.0f%%", Math.min(100.0, 100.0 * (ns - lastGpuNs) / (now - lastGpuTime)));
        }
        lastGpuNs = ns;
        lastGpuTime = now;
        return text;
    }

    private static native long nativeGpuNs();

    /** Whether some kernel file gives the GPU %; if not, the overlay needs the renderer's timestamps. */
    static boolean kernelGpuReadable() {
        for (String path : gpuCandidates()) {
            if (gpuPercent(path, readLine(path)) != null) return true;
        }
        return false;
    }

    /** The files tried for the GPU %, in order: the fixed ones, then each Mali/GPU devfreq node's load. */
    static List<String> gpuCandidates() {
        List<String> paths = new ArrayList<>(Arrays.asList(GPU_FILES));
        File[] devfreq = new File("/sys/class/devfreq").listFiles();
        if (devfreq != null) {
            for (File node : devfreq) {
                String name = node.getName().toLowerCase(Locale.ROOT);
                if (name.contains("mali") || name.contains("gpu")) paths.add(new File(node, "load").getPath());
            }
        }
        return paths;
    }

    /** The busy percentage in a GPU file's line, by the file's format; null when it is not one (or unreadable). */
    static Double gpuPercent(String path, String line) {
        if (line == null) return null;
        try {
            String text = line.trim();
            double percent;
            if (path.endsWith("/gpubusy")) {  // kgsl: busy and total time of the last window
                String[] fields = text.split("\\s+");
                long total = Long.parseLong(fields[1]);
                percent = total > 0 ? 100.0 * Long.parseLong(fields[0]) / total : 0;
            } else if (path.endsWith("/load")) {  // devfreq: "42@800000000Hz"
                percent = Double.parseDouble(text.split("@")[0].trim());
            } else {  // "42 %", "42", or GED's "loading block idle": the first number
                percent = Double.parseDouble(text.replace("%", " ").trim().split("\\s+")[0]);
            }
            return percent >= 0 && percent <= 100 ? percent : null;
        } catch (RuntimeException e) {
            return null;
        }
    }

    /** Each GPU file tried and what it gave, for the diagnostic report (phones where the overlay shows "--"). */
    static String gpuProbeReport() {
        StringBuilder out = new StringBuilder();
        for (String path : gpuCandidates()) {
            File file = new File(path);
            String line = readLine(path);
            out.append("  ").append(path).append(": ");
            if (!file.exists()) {
                out.append("não existe");
            } else if (line == null) {
                out.append("existe, sem permissão de leitura");
            } else {
                Double percent = gpuPercent(path, line);
                String shown = line.length() > 60 ? line.substring(0, 60) + "…" : line;
                out.append('"').append(shown.trim()).append('"')
                        .append(percent != null ? String.format(Locale.ROOT, " -> %.0f %%", percent) : " -> não é um valor");
            }
            out.append('\n');
        }
        return out.toString();
    }

    // utime + stime (fields 14 and 15) of a /proc stat line, -1 if unreadable. The name in parentheses may hold
    // spaces, so the fields are counted after its closing parenthesis.
    private static long ticksOf(String stat) {
        if (stat == null) return -1;
        int end = stat.lastIndexOf(')');
        if (end < 0) return -1;
        String[] fields = stat.substring(end + 2).split(" ");
        if (fields.length < 13) return -1;
        try {
            return Long.parseLong(fields[11]) + Long.parseLong(fields[12]);
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private static String nameOf(String stat) {
        int start = stat.indexOf('(');
        int end = stat.lastIndexOf(')');
        return start >= 0 && end > start ? stat.substring(start + 1, end) : "?";
    }

    private static String readLine(String path) {
        try (BufferedReader reader = new BufferedReader(new FileReader(path))) {
            return reader.readLine();
        } catch (IOException | SecurityException e) {
            return null;
        }
    }
}
