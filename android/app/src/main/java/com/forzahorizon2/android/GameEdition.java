package com.forzahorizon2.android;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.security.MessageDigest;

/**
 * Which edition of the game default.xex is.
 *
 * The recompiled code inside the APK comes from the USA executable (docs/fh2_disco.md). Another edition has its
 * code at other addresses and would not run, so the launcher checks the SHA-256 of default.xex before starting.
 */
final class GameEdition {
    static final String SUPPORTED_SHA256 = "516b04d7c69313cc581062e44030af3636f689e7f09eed1a709e4319f031feb7";
    static final String SUPPORTED_NAME = "USA (En, Ja, Fr, De, Es, It, Pt, Zh, Pl, Ru)";
    private static final String PREFS = "fh2_edition";

    final String sha256;

    private GameEdition(String sha256) {
        this.sha256 = sha256;
    }

    boolean supported() {
        return SUPPORTED_SHA256.equals(sha256);
    }

    /** The edition of the game folder's default.xex, or null if it cannot be read. Cached by size and date. */
    static GameEdition of(Context context, File gameRoot) {
        File xex = new File(gameRoot, "default.xex");
        if (!xex.isFile()) return null;
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String key = xex.length() + ":" + xex.lastModified();
        String sha = key.equals(prefs.getString("key", "")) ? prefs.getString("sha256", null) : null;
        if (sha == null) {
            try (InputStream in = new FileInputStream(xex)) {
                MessageDigest digest = MessageDigest.getInstance("SHA-256");
                byte[] buffer = new byte[1 << 16];
                for (int n; (n = in.read(buffer)) > 0; ) digest.update(buffer, 0, n);
                StringBuilder hex = new StringBuilder();
                for (byte b : digest.digest()) hex.append(String.format("%02x", b));
                sha = hex.toString();
            } catch (Exception e) {
                return null;
            }
            prefs.edit().putString("key", key).putString("sha256", sha).apply();
        }
        return new GameEdition(sha);
    }
}
