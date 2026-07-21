package com.goemon64.recomp;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Copies the read-only APK assets (fonts, .rml/.rcss UI, recompcontrollerdb.txt)
 * into app-private storage, where the native runtime reads them via
 * get_program_path()/get_asset_path(). Re-extracts when the app version changes.
 */
final class AssetInstaller {
    private static final String TAG = "Goemon64";

    private AssetInstaller() {}

    /**
     * @return true if the assets are present for this version (already extracted
     *         or freshly extracted), false if extraction failed. On failure the
     *         stamp is deliberately left unwritten so the next launch retries.
     *         Callers that can refuse (LauncherActivity) should not launch the
     *         game on false — the native side reads these assets immediately and
     *         crashes opaquely if they are missing/partial.
     */
    static boolean installIfNeeded(Context ctx, File dataDir) {
        String version;
        try {
            version = ctx.getPackageManager()
                    .getPackageInfo(ctx.getPackageName(), 0).versionName;
        } catch (Exception e) {
            version = "unknown";
        }

        File stamp = new File(dataDir, ".assets_version");
        if (stamp.exists() && version.equals(readStamp(stamp))) {
            return true; // already extracted for this version
        }

        AssetManager am = ctx.getAssets();
        try {
            // "assets" (the UI tree) and the loose controller DB are the roots the
            // native runtime looks for. Everything else in the APK asset dir
            // (webkit/, images/, etc.) is Android's and must be skipped.
            copyAsset(am, "assets", dataDir);
            copyAsset(am, "recompcontrollerdb.txt", dataDir);
            writeStamp(stamp, version);
            Log.i(TAG, "Assets extracted for version " + version);
            return true;
        } catch (IOException e) {
            Log.e(TAG, "Asset extraction failed", e);
            return false;
        }
    }

    private static void copyAsset(AssetManager am, String path, File outRoot) throws IOException {
        String[] children;
        try {
            children = am.list(path);
        } catch (IOException e) {
            children = null;
        }

        if (children != null && children.length > 0) {
            // Directory: recurse.
            File dir = new File(outRoot, path);
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
            for (String child : children) {
                copyAsset(am, path + "/" + child, outRoot);
            }
        } else {
            // File: copy bytes.
            File outFile = new File(outRoot, path);
            File parent = outFile.getParentFile();
            if (parent != null && !parent.exists()) {
                //noinspection ResultOfMethodCallIgnored
                parent.mkdirs();
            }
            try (InputStream in = am.open(path);
                 OutputStream out = new FileOutputStream(outFile)) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) != -1) {
                    out.write(buf, 0, n);
                }
            }
        }
    }

    private static String readStamp(File stamp) {
        // Read the whole file — a fixed-size single read() would truncate a long
        // version string (and isn't guaranteed to fill the buffer anyway), making
        // the equals() below fail forever and re-extract ~45 MB every launch.
        try {
            return new String(java.nio.file.Files.readAllBytes(stamp.toPath()),
                    java.nio.charset.StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeStamp(File stamp, String version) {
        try (OutputStream out = new FileOutputStream(stamp)) {
            out.write(version.getBytes("UTF-8"));
        } catch (IOException e) {
            Log.e(TAG, "Failed to write asset version stamp", e);
        }
    }
}
