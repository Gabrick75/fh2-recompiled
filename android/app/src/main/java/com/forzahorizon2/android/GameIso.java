package com.forzahorizon2.android;

import android.content.Context;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.system.Os;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

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
            return tr(R.string.the_file_is_not_an_xbox);
        } catch (IOException e) {
            return tr(R.string.could_not_read_the_file) + e.getMessage();
        }
    }

    /**
     * Where a file of the image is: {offset, size} in bytes from the image's start, or null. The path is relative
     * to the disc's root ("media/ui/videos/PressStart.wmv"), any case. GDFX directories are binary trees of entries
     * (left, right, sector, size, attributes, name); each one walked whole, they are a few sectors.
     */
    static long[] find(File iso, String path) {
        try (RandomAccessFile file = new RandomAccessFile(iso, "r")) {
            byte[] read = new byte[MAGIC.length];
            for (long partition : PARTITIONS) {
                long at = partition + DESCRIPTOR;
                if (at + 28 > file.length()) continue;
                file.seek(at);
                file.readFully(read);
                if (!java.util.Arrays.equals(read, MAGIC)) continue;
                byte[] root = new byte[8];
                file.readFully(root);
                long sector = le32(root, 0), size = le32(root, 4);
                boolean directory = true;
                for (String name : path.split("/")) {
                    if (!directory || size <= 0 || size > (1 << 20)) return null;
                    byte[] table = new byte[(int) size];
                    file.seek(partition + sector * 2048);
                    file.readFully(table);
                    int entry = findEntry(table, 0, name);
                    if (entry < 0) return null;
                    sector = le32(table, entry + 4);
                    size = le32(table, entry + 8);
                    directory = (table[entry + 12] & 0x10) != 0;
                }
                return directory ? null : new long[] {partition + sector * 2048, size};
            }
        } catch (IOException e) {
            // unreadable: as if not there
        }
        return null;
    }

    private static int findEntry(byte[] table, int dword, String name) {
        int at = dword * 4;
        if (at + 14 > table.length) return -1;
        int left = (table[at] & 0xFF) | (table[at + 1] & 0xFF) << 8;
        int right = (table[at + 2] & 0xFF) | (table[at + 3] & 0xFF) << 8;
        if (left == 0xFFFF) return -1;  // an empty directory's padding
        int length = table[at + 13] & 0xFF;
        if (at + 14 + length > table.length) return -1;
        String entry = new String(table, at + 14, length, StandardCharsets.ISO_8859_1);
        if (entry.equalsIgnoreCase(name)) return at;
        int found = left != 0 ? findEntry(table, left, name) : -1;
        return found >= 0 || right == 0 ? found : findEntry(table, right, name);
    }

    private static long le32(byte[] b, int at) {
        return (b[at] & 0xFFL) | (b[at + 1] & 0xFFL) << 8 | (b[at + 2] & 0xFFL) << 16 | (b[at + 3] & 0xFFL) << 24;
    }

    /**
     * The file path behind what the system file picker returned, or null. First from the document id, then from
     * the file the picker opens, for the ids that carry no path: the Downloads shortcut ("msf:123", "123"), the
     * media categories ("document:123") and other apps' file managers shown in the picker (issue #1).
     */
    static File pathOf(Context context, Uri uri) {
        File named = pathInId(uri);
        if (named != null && named.isFile()) return named;
        File opened = pathOfOpened(context, uri);
        return opened != null ? opened : named;
    }

    /**
     * The path the document id spells: internal storage and SD cards ("primary:jogos/fh2.iso", "1234-ABCD:fh2.iso"),
     * the picker's Documents shortcut ("home:fh2.iso") and the Downloads list when it gives the path ("raw:/...").
     */
    private static File pathInId(Uri uri) {
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
            File base;
            if ("primary".equalsIgnoreCase(volume)) base = Environment.getExternalStorageDirectory();
            else if ("home".equalsIgnoreCase(volume)) {
                base = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS);
            } else base = new File("/storage/" + volume);
            return new File(base, relative);
        }
        if ("com.android.providers.downloads.documents".equals(authority) && id.startsWith("raw:")) {
            return new File(id.substring(4));
        }
        return null;
    }

    /**
     * The path of the file the picker's provider opens, read from /proc/self/fd, when it is a file this app can
     * open by path (all-files access) and of the same size. A file served from the cloud or through a pipe has no
     * such path.
     */
    private static File pathOfOpened(Context context, Uri uri) {
        try (ParcelFileDescriptor descriptor = context.getContentResolver().openFileDescriptor(uri, "r")) {
            if (descriptor == null) return null;
            String target = Os.readlink("/proc/self/fd/" + descriptor.getFd());
            File file = new File(storagePath(target));
            long size = Os.fstat(descriptor.getFileDescriptor()).st_size;
            return file.isFile() && file.canRead() && file.length() == size ? file : null;
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * The /storage path of a path seen from the provider's side: MediaStore's FUSE mount (/mnt/user/0/emulated/0/,
     * /mnt/user/0/1234-ABCD/), its lower file system (/data/media/0/) and SD cards (/mnt/media_rw/1234-ABCD/).
     */
    private static String storagePath(String path) {
        Matcher m = Pattern.compile("^/mnt/user/\\d+/emulated/(\\d+)/(.*)").matcher(path);
        if (m.matches()) return "/storage/emulated/" + m.group(1) + "/" + m.group(2);
        m = Pattern.compile("^/data/media/(\\d+)/(.*)").matcher(path);
        if (m.matches()) return "/storage/emulated/" + m.group(1) + "/" + m.group(2);
        m = Pattern.compile("^/mnt/(?:user/\\d+|media_rw)/([^/]+)/(.*)").matcher(path);
        if (m.matches() && !"emulated".equals(m.group(1))) return "/storage/" + m.group(1) + "/" + m.group(2);
        return path;
    }
}
