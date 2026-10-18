package com.forzahorizon2.android;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.TimeUnit;

/**
 * Extra diagnostics for rooted phones ("Diagnóstico com root"): what Android hides from ordinary apps, read through
 * su and only read. Nothing on the phone is changed, the permission is not kept for anything else, and other apps'
 * crash files are left out. Each command has a time and size limit; one that fails is noted and the rest go on.
 */
final class RootDiagnostics {
    private static final String PACKAGE = "com.forzahorizon2.android";

    // GPU load, clock and limits, one path per line (no brace expansion: it is not in every Android shell).
    private static final String[] GPU_SENSOR_FILES = {
            "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", "/sys/class/kgsl/kgsl-3d0/gpubusy",
            "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", "/sys/class/kgsl/kgsl-3d0/devfreq/max_freq",
            "/sys/class/kgsl/kgsl-3d0/devfreq/min_freq", "/sys/class/kgsl/kgsl-3d0/max_gpuclk",
            "/sys/class/kgsl/kgsl-3d0/thermal_pwrlevel", "/sys/class/kgsl/kgsl-3d0/throttling",
            "/sys/kernel/ged/hal/gpu_utilization", "/sys/kernel/ged/hal/current_freqency",
            "/sys/module/ged/parameters/gpu_loading", "/proc/gpufreqv2/gpufreq_status",
            "/sys/kernel/gpu/gpu_busy", "/sys/kernel/gpu/gpu_clock", "/sys/kernel/gpu/gpu_max_clock",
            "/sys/class/misc/mali0/device/utilization",
    };

    private RootDiagnostics() {
    }

    /** su -c id: the first call shows the root manager's prompt (Magisk, KernelSU). True when it ran as uid 0. */
    static boolean available() {
        String id = run("id", 30, 4096);
        return id != null && id.contains("uid=0");
    }

    /** A command as root, its output and errors, at most limit bytes; null when su is missing or it failed. */
    static String run(String command, int timeoutSeconds, int limit) {
        Process process = null;
        try {
            process = new ProcessBuilder("su", "-c", command).redirectErrorStream(true).start();
            Process running = process;
            Thread watchdog = new Thread(() -> {
                try {
                    if (!running.waitFor(timeoutSeconds, TimeUnit.SECONDS)) running.destroyForcibly();
                } catch (InterruptedException e) {
                    running.destroyForcibly();
                    Thread.currentThread().interrupt();
                }
            }, "RootDiagnosticsTimeout");
            watchdog.setDaemon(true);
            watchdog.start();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            try (InputStream input = process.getInputStream()) {
                byte[] buffer = new byte[16384];
                for (int n; (n = input.read(buffer)) > 0; ) {
                    if (out.size() < limit) out.write(buffer, 0, Math.min(n, limit - out.size()));
                }
            }
            process.waitFor(2, TimeUnit.SECONDS);
            return out.toString("UTF-8");
        } catch (IOException e) {
            return null;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return null;
        } finally {
            if (process != null) process.destroy();
        }
    }

    /**
     * The root/ entries of the report (name -> content) and, under "", a summary line for relatorio.txt.
     * Call off the UI thread, after available().
     */
    static Map<String, byte[]> collect() {
        Map<String, byte[]> files = new LinkedHashMap<>();
        StringBuilder summary = new StringBuilder();

        // 1. Our native crashes: the full backtrace of every thread, registers and memory map.
        String list = run("for f in $(ls -t /data/tombstones/tombstone_* 2>/dev/null | grep -v '\\.pb$' | head -40); "
                + "do grep -q '" + PACKAGE + "' \"$f\" && echo \"$f\"; done | head -3", 20, 8192);
        int tombstones = 0;
        if (list != null) {
            for (String path : list.split("\n")) {
                path = path.trim();
                if (!path.startsWith("/data/tombstones/")) continue;
                String content = run("cat '" + path + "'", 15, 512 * 1024);
                if (content != null) {
                    files.put("root/tombstones/" + path.substring(path.lastIndexOf('/') + 1) + ".txt", bytes(content));
                    ++tombstones;
                }
            }
        }
        summary.append("tombstones do jogo: ").append(tombstones);

        // ANR traces of ours (the newest only).
        String anr = run("for f in $(ls -t /data/anr/* 2>/dev/null | head -20); do grep -q 'Cmd line: " + PACKAGE
                + "' \"$f\" && echo \"$f\" && break; done", 20, 4096);
        if (anr != null && anr.trim().startsWith("/data/anr/")) {
            String path = anr.trim().split("\n")[0].trim();
            String content = run("cat '" + path + "'", 15, 1024 * 1024);
            if (content != null) {
                files.put("root/anr/" + path.substring(path.lastIndexOf('/') + 1) + ".txt", bytes(content));
                summary.append(", ANR: 1");
            }
        }

        // 2. The kernel's log: GPU faults (kgsl, mali, ged), the low memory killer, thermal limits.
        String dmesg = run("dmesg 2>&1 | tail -n 6000", 20, 1024 * 1024);
        if (dmesg != null) {
            files.put("root/dmesg.txt", bytes(dmesg));
            summary.append(", dmesg ok");
        } else {
            summary.append(", dmesg falhou");
        }

        // 3. Every logcat buffer (system, events, radio...), not only this app's main and crash ones.
        String logcat = run("logcat -b all -d -v threadtime -t 20000", 30, 3 * 1024 * 1024);
        if (logcat != null) {
            files.put("root/logcat-completo.txt", bytes(logcat));
            summary.append(", logcat completo ok");
        } else {
            summary.append(", logcat completo falhou");
        }

        // 4. Sensors without the blocks ordinary apps hit: GPU load and clocks (Adreno, MediaTek GED, Mali), every
        // thermal zone, each CPU cluster's clock, cap and governor.
        StringBuilder gpuFiles = new StringBuilder();
        for (String path : GPU_SENSOR_FILES) {
            gpuFiles.append("[ -e ").append(path).append(" ] && echo \"").append(path).append(": $(head -c 200 ")
                    .append(path).append(" 2>&1)\"; ");
        }
        String sensors = run("echo '== GPU'; " + gpuFiles
                + "for d in /sys/class/devfreq/*; do echo \"$d: atual $(cat $d/cur_freq 2>/dev/null) min "
                + "$(cat $d/min_freq 2>/dev/null) max $(cat $d/max_freq 2>/dev/null) carga $(cat $d/load 2>/dev/null) "
                + "governador $(cat $d/governor 2>/dev/null)\"; done; "
                + "echo '== Termico (miligraus)'; for z in /sys/class/thermal/thermal_zone*; do "
                + "echo \"$(cat $z/type 2>/dev/null): $(cat $z/temp 2>/dev/null)\"; done; "
                + "echo '== CPU (kHz)'; for p in /sys/devices/system/cpu/cpufreq/policy*; do "
                + "echo \"$p: nucleos $(cat $p/related_cpus 2>/dev/null) atual $(cat $p/scaling_cur_freq 2>/dev/null) "
                + "limite $(cat $p/scaling_max_freq 2>/dev/null) de $(cat $p/cpuinfo_max_freq 2>/dev/null) "
                + "governador $(cat $p/scaling_governor 2>/dev/null)\"; done", 20, 256 * 1024);
        if (sensors != null) {
            files.put("root/sensores.txt", bytes(sensors));
            summary.append(", sensores ok");
        }

        // 5. The system: every property, the compositor and display modes, GPU and thermal services, this app's
        // frame and memory stats.
        String props = run("getprop", 15, 512 * 1024);
        if (props != null) files.put("root/getprop.txt", bytes(props));
        String dumpsys = run("for s in 'SurfaceFlinger' 'gpu' 'thermalservice' 'gfxinfo " + PACKAGE + "' 'meminfo "
                + PACKAGE + "'; do echo \"===== dumpsys $s\"; dumpsys $s 2>&1 | head -c 400000; done", 40,
                2 * 1024 * 1024);
        if (dumpsys != null) files.put("root/dumpsys.txt", bytes(dumpsys));
        String meminfo = run("cat /proc/meminfo", 10, 64 * 1024);
        if (meminfo != null) files.put("root/meminfo.txt", bytes(meminfo));
        summary.append(props != null && dumpsys != null ? ", sistema ok" : ", sistema incompleto");

        files.put("", bytes(summary.toString()));
        return files;
    }

    /**
     * The performance profile of a running game ("Gravar perfil de desempenho"): after waitSeconds of play, 10 s of
     * simpleperf on this process, summarized by thread and by function into <session>_perfil.txt. Only measures;
     * the raw recording is deleted. Runs on a thread of its own.
     */
    static void profileLater(int pid, String sessionBase, int waitSeconds) {
        Thread thread = new Thread(() -> {
            try {
                Thread.sleep(waitSeconds * 1000L);
            } catch (InterruptedException e) {
                return;
            }
            String data = "/data/local/tmp/fh2_perfil.data";
            StringBuilder out = new StringBuilder("== simpleperf: 10 s do jogo (pid " + pid + ") depois de "
                    + waitSeconds + " s de partida\n");
            String record = run("simpleperf record -p " + pid + " --duration 10 -f 1000 -o " + data + " 2>&1", 60,
                    64 * 1024);
            out.append(record == null ? "sem root ou sem simpleperf\n" : record).append('\n');
            if (record != null) {
                String threads = run("simpleperf report -i " + data + " --sort comm --percent-limit 0.5 2>&1", 60,
                        256 * 1024);
                String functions = run("simpleperf report -i " + data
                        + " --sort comm,dso,symbol --percent-limit 0.3 2>&1", 90, 1024 * 1024);
                out.append("== por thread\n").append(threads != null ? threads : "falhou (simpleperf report)\n")
                        .append("\n== por thread, biblioteca e funcao\n")
                        .append(functions != null ? functions : "falhou (simpleperf report)\n").append('\n');
                run("rm -f " + data, 10, 1024);
            }
            try (java.io.FileWriter writer = new java.io.FileWriter(sessionBase + "_perfil.txt")) {
                writer.write(out.toString());
            } catch (IOException ignored) {
                // Nowhere to write it.
            }
        }, "RootProfile");
        thread.setDaemon(true);
        thread.start();
    }

    private static byte[] bytes(String text) {
        return text.getBytes(StandardCharsets.UTF_8);
    }
}
