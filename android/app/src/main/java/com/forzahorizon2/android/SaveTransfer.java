package com.forzahorizon2.android;

import android.content.Context;
import android.os.Environment;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Moving the player's progress between phones. A save is what GameActivity.backUpSaves copies before every run: the
 * profile folders (16 hex digits, the XUID, with the saves of every title inside) and achievements.
 *
 * Exports go to Internal storage/jogos/ForzaHorizon2-saves-exportados/<date> (never pruned); the automatic backups
 * stay in ForzaHorizon2-saves/<date>. Importing takes either: copy one of those folders to another phone and import
 * it there. From adb (tests): MainActivity with --es fh2.importar_save <folder name or absolute path>.
 */
final class SaveTransfer {
    private static final String TAG = "FH2Save";

    private SaveTransfer() {
    }

    static File userRoot(Context context) {
        return new File(new File(context.getFilesDir(), "fh2"), "user");
    }

    static File backupsFolder() {
        return new File(Environment.getExternalStorageDirectory(), MainActivity.GAME_FOLDER_NAME + "-saves");
    }

    static File exportsFolder() {
        return new File(Environment.getExternalStorageDirectory(), MainActivity.GAME_FOLDER_NAME + "-saves-exportados");
    }

    private static boolean isSaveEntry(File f) {
        return f.isDirectory() && (f.getName().matches("[0-9A-Fa-f]{16}") || f.getName().equals("achievements"));
    }

    /** The save folders that can be imported, newest first: exports, then automatic backups. */
    static List<File> importable() {
        List<File> all = new ArrayList<>();
        for (File folder : new File[] {exportsFolder(), backupsFolder()}) {
            File[] found = folder.listFiles(f -> f.isDirectory() && hasSave(f));
            if (found == null) continue;
            Arrays.sort(found, (a, b) -> b.getName().compareTo(a.getName()));  // the names are dates
            all.addAll(Arrays.asList(found));
        }
        return all;
    }

    static boolean hasSave(File folder) {
        File[] entries = folder.listFiles(SaveTransfer::isSaveEntry);
        return entries != null && entries.length > 0;
    }

    /** Copies the current progress to a new export folder and returns it. */
    static File export(Context context) throws IOException {
        File[] entries = userRoot(context).listFiles(SaveTransfer::isSaveEntry);
        if (entries == null || entries.length == 0) {
            throw new IOException("ainda não há save");
        }
        String stamp = new java.text.SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", java.util.Locale.ROOT)
                .format(new java.util.Date());
        File target = new File(exportsFolder(), stamp);
        for (File entry : entries) {
            GameActivity.copyTree(entry, new File(target, entry.getName()));
        }
        Log.i(TAG, "Save exportado para " + target);
        return target;
    }

    /** Replaces the current progress with the save in `source` (an export or a backup folder). */
    static void importFrom(Context context, File source) throws IOException {
        File[] entries = source.listFiles(SaveTransfer::isSaveEntry);
        if (entries == null || entries.length == 0) {
            throw new IOException("a pasta não tem um save: " + source);
        }
        File root = userRoot(context);
        if (!root.mkdirs() && !root.isDirectory()) {
            throw new IOException("mkdir " + root);
        }
        File[] current = root.listFiles(SaveTransfer::isSaveEntry);
        if (current != null) {
            for (File entry : current) {
                GameActivity.deleteTree(entry);
            }
        }
        for (File entry : entries) {
            GameActivity.copyTree(entry, new File(root, entry.getName()));
        }
        Log.i(TAG, "Save importado de " + source);
    }

    /** A folder name inside the exports or backups folder, or an absolute path. */
    static File resolve(String name) {
        File direct = new File(name);
        if (direct.isAbsolute()) return direct;
        File exported = new File(exportsFolder(), name);
        return exported.isDirectory() ? exported : new File(backupsFolder(), name);
    }
}
