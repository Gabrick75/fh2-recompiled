package com.forzahorizon2.android;

import android.app.ActivityManager;
import android.content.Context;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * GPU, CPU and RAM use for the in-game overlay ("Mostrar uso de GPU, CPU e RAM"). Call sample() about once a second,
 * off the UI thread: it reads a few dozen /proc files.
 *
 * - GPU: the kernel driver's busy percentage (Adreno kgsl, or Mali), "--" where the phone does not let apps read it.
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
    };

    private final ActivityManager activityManager;
    private final int cores = Math.max(1, Runtime.getRuntime().availableProcessors());
    private String gpuFile;
    private boolean gpuLogged;
    private long lastProcessTicks = -1;
    private long lastTime;
    private Map<String, Long> lastThreadTicks = new HashMap<>();

    UsageMonitor(Context context) {
        activityManager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
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
            ram += String.format(Locale.ROOT, " (livre %.1f/%.1f)", info.availMem / (double) (1 << 30),
                    info.totalMem / (double) (1 << 30));
        }

        return gpu() + "  " + cpu + "  " + ram;
    }

    private String gpu() {
        if (gpuFile == null) {
            for (String path : GPU_FILES) {
                if (readLine(path) != null) {
                    gpuFile = path;
                    break;
                }
            }
            if (!gpuLogged) {
                gpuLogged = true;
                Log.i(TAG, "GPU use read from " + (gpuFile != null ? gpuFile : "nowhere (no readable file)"));
            }
            if (gpuFile == null) return "GPU --";
        }
        String line = readLine(gpuFile);
        if (line == null) return "GPU --";
        String[] fields = line.trim().replace("%", "").trim().split("\\s+");
        try {
            double percent;
            if (fields.length >= 2) {  // gpubusy: busy and total time of the last window
                long total = Long.parseLong(fields[1]);
                percent = total > 0 ? 100.0 * Long.parseLong(fields[0]) / total : 0;
            } else {
                percent = Double.parseDouble(fields[0]);
            }
            return String.format(Locale.ROOT, "GPU %.0f%%", percent);
        } catch (NumberFormatException e) {
            return "GPU --";
        }
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
