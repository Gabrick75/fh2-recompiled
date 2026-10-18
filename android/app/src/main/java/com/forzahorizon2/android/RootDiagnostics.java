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

        files.put("", bytes(summary.toString()));
        return files;
    }

    private static byte[] bytes(String text) {
        return text.getBytes(StandardCharsets.UTF_8);
    }
}
