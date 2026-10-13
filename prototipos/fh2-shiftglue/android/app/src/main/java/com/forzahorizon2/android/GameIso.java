package com.forzahorizon2.android;

import android.content.Context;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;

import static com.forzahorizon2.android.AppLanguage.tr;

/**
 * The game read straight from its disc image (.iso), without extracting or copying it: the game mounts it with the
 * SDK's DiscImageDevice (runtime.cpp, SetupVfs) when game_data_root is a file. The launcher only keeps the path
 * (GameOptions.GAME_ISO); the app has all-files access, so the game opens it by path like the extracted folder.
 */
final class GameIso {
    // Where the game partition starts on the disc kinds DiscImageDevice knows (XGD1, XGD2, XGD3, a plain xiso), and
    // the GDFX volume descriptor 32 sectors in.
    private static final long[] PARTITIONS = {0x0L, 0xFDA0000L, 0x2080000L, 0x18300000L};
    private static final long DESCRIPTOR = 32 * 2048;
    private static final byte[] MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);

    private GameIso() {
    }

    /** The chosen image, or null when none is chosen or it is gone. */
    static File chosen(Context context) {
        String path = GameOptions.get(context, GameOptions.GAME_ISO);
        if (path.isEmpty()) return null;
        File iso = new File(path);
        return iso.isFile() ? iso : null;
    }

    /** What the game mounts: the chosen image, or the extracted folder. */
    static File gameDataRoot(Context context) {
        File iso = chosen(context);
        return iso != null ? iso : MainActivity.gameRoot();
    }

    /** Null if the file is an Xbox 360 disc image, otherwise why not (for the player). */
    static String check(File iso) {
        try (RandomAccessFile file = new RandomAccessFile(iso, "r")) {
            byte[] read = new byte[MAGIC.length];
            for (long partition : PARTITIONS) {
                long at = partition + DESCRIPTOR;
                if (at + read.length > file.length()) continue;
                file.seek(at);
                file.readFully(read);
                if (java.util.Arrays.equals(read, MAGIC)) return null;
            }
            return tr("o arquivo não é uma imagem de disco do Xbox 360", "the file is not an Xbox 360 disc image");
        } catch (IOException e) {
            return tr("não foi possível ler o arquivo: ", "could not read the file: ") + e.getMessage();
        }
    }

    /**
     * The file path behind what the system file picker returned, or null. Internal storage and SD cards
     * ("primary:jogos/fh2.iso", "1234-ABCD:fh2.iso") and the Downloads list when it gives the path ("raw:/...").
     */
    static File pathOf(Uri uri) {
        if ("file".equals(uri.getScheme())) return new File(uri.getPath());
        String id;
        try {
            id = DocumentsContract.getDocumentId(uri);
        } catch (IllegalArgumentException e) {
            return null;
        }
        String authority = uri.getAuthority();
        if ("com.android.externalstorage.documents".equals(authority)) {
            int colon = id.indexOf(':');
            if (colon < 0) return null;
            String volume = id.substring(0, colon), relative = id.substring(colon + 1);
            File base = "primary".equalsIgnoreCase(volume) ? Environment.getExternalStorageDirectory()
                    : new File("/storage/" + volume);
            return new File(base, relative);
        }
        if ("com.android.providers.downloads.documents".equals(authority) && id.startsWith("raw:")) {
            return new File(id.substring(4));
        }
        return null;
    }
}
