package com.forzahorizon2.android;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The game's achievements for the launcher's page. The list lives in the game executable's XDBF, which only the game
 * reads: on every start the SDK writes it to user/achievements/catalog_<title>.json, with the icons in icons/<image
 * id>.png (kernel_state.cpp, ExportAchievementCatalog). The unlocks are the SDK's own save next to it, <title>.toml
 * ([unlocked.<id>] with the unlock's filetime). So the page is empty until the game has been opened once.
 */
final class Achievements {
    // XDBF achievement flag: the locked description may be shown. Without it the achievement is secret until unlocked.
    private static final int FLAG_SHOW_LOCKED = 0x8;

    static final class Entry {
        final int id;
        final String label;
        final String description;
        final String lockedDescription;
        final int gamerscore;
        final File icon;          // null if the game had none
        final boolean secret;
        long unlockedAtMillis;    // 0 = locked

        Entry(JSONObject o, File icons) {
            id = o.optInt("id");
            label = o.optString("label");
            description = o.optString("description");
            lockedDescription = o.optString("locked_description");
            gamerscore = o.optInt("gamerscore");
            int image = o.optInt("image_id");
            File file = new File(icons, image + ".png");
            icon = image != 0 && file.isFile() ? file : null;
            secret = (o.optInt("flags") & FLAG_SHOW_LOCKED) == 0;
        }

        boolean unlocked() {
            return unlockedAtMillis != 0;
        }
    }

    private Achievements() {
    }

    static File folder(Context context) {
        return new File(SaveTransfer.userRoot(context), "achievements");
    }

    /** Every achievement of the game, with its unlock time; empty if the game has not written the catalog yet. */
    static List<Entry> load(Context context) {
        List<Entry> entries = new ArrayList<>();
        File dir = folder(context);
        File[] catalogs = dir.listFiles((d, name) -> name.startsWith("catalog_") && name.endsWith(".json"));
        if (catalogs == null || catalogs.length == 0) return entries;
        try {
            JSONObject root = new JSONObject(read(catalogs[0]));
            String title = root.optString("title_id");
            JSONArray list = root.optJSONArray("achievements");
            if (list == null) return entries;
            File icons = new File(dir, "icons");
            for (int i = 0; i < list.length(); i++) entries.add(new Entry(list.getJSONObject(i), icons));
            Map<Integer, Long> unlocked = unlocks(new File(dir, title + ".toml"));
            for (Entry e : entries) {
                Long filetime = unlocked.get(e.id);
                if (filetime != null) e.unlockedAtMillis = Math.max(1, filetimeToMillis(filetime));
            }
        } catch (Exception e) {
            entries.clear();  // a half-written or foreign file: show nothing rather than garbage
        }
        return entries;
    }

    // The SDK's unlock save (achievement_manager.cpp, SaveUnlockState): "[unlocked.<id>]" then "filetime = <n>".
    private static Map<Integer, Long> unlocks(File file) {
        Map<Integer, Long> unlocked = new HashMap<>();
        if (!file.isFile()) return unlocked;
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            Integer current = null;
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.startsWith("[unlocked.") && line.endsWith("]")) {
                    current = Integer.parseInt(line.substring(10, line.length() - 1).trim());
                    unlocked.put(current, 0L);
                } else if (current != null && line.startsWith("filetime")) {
                    int equals = line.indexOf('=');
                    if (equals > 0) unlocked.put(current, Long.parseLong(line.substring(equals + 1).trim()));
                }
            }
        } catch (IOException | NumberFormatException e) {
            // what was read so far stays
        }
        return unlocked;
    }

    // Windows FILETIME (100 ns since 1601) to Unix milliseconds.
    private static long filetimeToMillis(long filetime) {
        return filetime / 10000L - 11644473600000L;
    }

    private static String read(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            byte[] bytes = new byte[(int) file.length()];
            int n = 0;
            while (n < bytes.length) {
                int r = in.read(bytes, n, bytes.length - n);
                if (r < 0) break;
                n += r;
            }
            return new String(bytes, 0, n, StandardCharsets.UTF_8);
        }
    }
}
