package com.goemon64.recomp;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Helpers for documents the user picks through the Storage Access Framework.
 *
 * <p>SAF hands out {@code content://} URIs, and native code wants a path it can
 * open with the C library, so anything crossing that boundary has to be copied
 * into app-private storage first. That copy is what {@link #copyToCache} does.
 */
final class SafFiles {
    private static final String TAG = "Goemon64";

    /** Subdirectory of the cache dir holding copies of picked documents. */
    private static final String PICKED_DIR = "picked";

    /**
     * Refuse to copy anything larger than this. Nothing the app asks a user to
     * pick — a mod, a Vulkan driver — comes close, and without a cap a stray
     * selection could fill the device's storage before anyone noticed.
     */
    private static final long MAX_COPY_BYTES = 512L * 1024L * 1024L;

    private SafFiles() {}

    /**
     * The document's own file name, which callers need because it carries the
     * extension the consumer dispatches on (a mod is identified by {@code .nrm},
     * a driver package by being a zip). Falls back to the URI's last path segment
     * for providers that refuse the query.
     */
    static String displayName(Context context, Uri uri) {
        try (Cursor cursor = context.getContentResolver()
                .query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) {
                    String name = cursor.getString(column);
                    if (name != null && !name.isEmpty()) {
                        return new File(name).getName();
                    }
                }
            }
        } catch (RuntimeException e) {
            // Some providers refuse this query; the fallback below is fine.
            Log.w(TAG, "no display name for " + uri, e);
        }
        String path = uri.getLastPathSegment();
        return (path == null || path.isEmpty()) ? "picked.bin" : new File(path).getName();
    }

    /**
     * Copy a picked document into the cache and return the file, keeping its
     * original name so extension-based dispatch on the native side still works.
     * Each document lands in its own numbered directory, because two picks in one
     * selection can share a name.
     */
    static File copyToCache(Context context, Uri uri, int index) throws IOException {
        File dir = new File(new File(context.getCacheDir(), PICKED_DIR), Integer.toString(index));
        deleteRecursively(dir);
        if (!dir.mkdirs()) {
            throw new IOException("cannot create " + dir);
        }
        File out = new File(dir, displayName(context, uri));

        long written = 0;
        try (InputStream in = context.getContentResolver().openInputStream(uri);
             OutputStream sink = new FileOutputStream(out)) {
            if (in == null) {
                throw new IOException("cannot open " + uri);
            }
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                written += n;
                if (written > MAX_COPY_BYTES) {
                    throw new IOException("picked file is too large");
                }
                sink.write(buf, 0, n);
            }
        } catch (IOException | RuntimeException e) {
            // A partial copy would be handed to a consumer as if it were whole.
            deleteRecursively(dir);
            throw (e instanceof IOException) ? (IOException) e : new IOException(e);
        }
        return out;
    }

    /**
     * Drop everything copied for a previous pick. Called when a new one starts
     * rather than when the old one is finished with, because the consumer is
     * native code and there is no point at which it reports being done.
     */
    static void clearCache(Context context) {
        deleteRecursively(new File(context.getCacheDir(), PICKED_DIR));
    }

    private static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursively(child);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }
}
