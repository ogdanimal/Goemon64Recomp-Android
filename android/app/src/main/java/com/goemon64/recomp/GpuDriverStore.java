package com.goemon64.recomp;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.provider.OpenableColumns;
import android.util.Log;

import androidx.annotation.Nullable;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * On-disk store for user-supplied Vulkan drivers, plus the state that lets us
 * recover from a bad one.
 *
 * <p>Everything lives under {@code getFilesDir()/gpu_driver}. That location is
 * not negotiable: {@code dlopen} refuses libraries on shared or SD storage, so
 * the {@link DataPaths} data directory — which may well be a removable card —
 * cannot hold a driver. It is also why this class does not consult DataPaths at
 * all.
 *
 * <p>Layout:
 * <pre>
 *   gpu_driver/
 *     drivers/&lt;id&gt;/&lt;libraryName&gt;   the driver itself
 *                  /driver.json          our normalised record of it
 *     state/active_id                    selected driver, absent = system driver
 *          /boot_pending                 a launch is in flight, see the latch below
 *          /last_device                  GPU name the renderer actually got
 *          /last_status                  loader outcome, written by native code
 *          /disabled_notice              why we turned a driver off
 *     tmp/                               scratch for libadrenotools below API 29
 * </pre>
 *
 * <p><b>State is plain files rather than SharedPreferences on purpose.</b> Native
 * code writes {@code last_device}, and files are a contract both sides can honour
 * without a JNI bridge in the other direction. It also means the whole feature —
 * drivers, selection and recovery state — can be reset by deleting one directory.
 *
 * <p><b>The crash latch.</b> A bad driver takes the app down before any UI exists,
 * and the launcher goes straight back into the game, so it is otherwise an
 * unbreakable crash loop with no way to reach a setting. Before launching with a
 * custom driver we write {@code boot_pending}; MainActivity clears it once the
 * process has stayed alive long enough to count as good. Finding it still there
 * on the next launch means the previous attempt died, so the driver is deselected
 * and a notice is left for the launcher to show.
 *
 * <p>The clear is on a timer rather than on the renderer reporting a working
 * device, because the Adreno fault this feature exists for kills the process
 * about a second <em>after</em> rendering starts — clearing at renderer setup
 * would disarm the latch immediately before the crash it is meant to catch.
 *
 * <p><b>What the latch does not cover:</b> a driver that survives that window and
 * then renders wrong or crashes later. Nothing can detect that automatically,
 * which is why driver selection is also reachable from its own launcher icon
 * rather than only from inside the game.
 */
public final class GpuDriverStore {
    private static final String TAG = "Goemon64";

    private static final String ROOT_DIR = "gpu_driver";
    private static final String DRIVERS_DIR = "drivers";
    private static final String STATE_DIR = "state";
    private static final String TMP_DIR = "tmp";

    private static final String RECORD_FILE = "driver.json";
    private static final String ACTIVE_FILE = "active_id";
    private static final String BOOT_PENDING_FILE = "boot_pending";
    private static final String LAST_DEVICE_FILE = "last_device";
    private static final String LAST_STATUS_FILE = "last_status";
    private static final String DISABLED_NOTICE_FILE = "disabled_notice";

    /** Refuse anything larger than this as obviously not a Vulkan driver. */
    private static final long MAX_IMPORT_BYTES = 256L * 1024L * 1024L;

    // Loader outcomes, written to state/last_status by android_glue.cpp. Must stay
    // in sync with the CustomDriverStatus values there.
    public static final int STATUS_NOT_ATTEMPTED = 0;
    public static final int STATUS_REQUESTED = 1;
    public static final int STATUS_OPEN_FAILED = 2;
    public static final int STATUS_NO_PROC_ADDR = 3;
    public static final int STATUS_BAD_ARGUMENTS = 4;

    private GpuDriverStore() {}

    /** A driver on disk, as described by its own metadata where it had any. */
    public static final class DriverInfo {
        public final String id;
        public final String name;
        public final String libraryName;
        public final String driverVersion;
        public final String vendor;
        public final int minApi;

        DriverInfo(String id, String name, String libraryName, String driverVersion, String vendor, int minApi) {
            this.id = id;
            this.name = name;
            this.libraryName = libraryName;
            this.driverVersion = driverVersion;
            this.vendor = vendor;
            this.minApi = minApi;
        }

        /** One line for a list row: version and vendor when the package supplied them. */
        public String subtitle() {
            StringBuilder sb = new StringBuilder();
            if (!driverVersion.isEmpty()) {
                sb.append(driverVersion);
            }
            if (!vendor.isEmpty()) {
                if (sb.length() > 0) {
                    sb.append(" • ");
                }
                sb.append(vendor);
            }
            if (sb.length() == 0) {
                sb.append(libraryName);
            }
            return sb.toString();
        }

        /** True when the package declares an API level this device does not meet. */
        public boolean isTooNewForThisDevice() {
            return minApi > 0 && Build.VERSION.SDK_INT < minApi;
        }
    }

    /** Import rejected for a reason worth showing the user verbatim. */
    public static final class ImportException extends Exception {
        ImportException(String message) {
            super(message);
        }
    }

    // ---------------------------------------------------------------- paths

    public static File root(Context context) {
        return new File(context.getFilesDir(), ROOT_DIR);
    }

    public static File driversDir(Context context) {
        return new File(root(context), DRIVERS_DIR);
    }

    public static File stateDir(Context context) {
        return new File(root(context), STATE_DIR);
    }

    /**
     * Scratch directory for libadrenotools. It only uses this below API 29, where
     * memfd may be unavailable; passing null there makes it attempt memfd and
     * return null if the kernel lacks it. minSdk is 28, so without this a device
     * at exactly 28 would silently fall through to the system driver.
     */
    public static File tmpDir(Context context) {
        return new File(root(context), TMP_DIR);
    }

    public static File directoryFor(Context context, DriverInfo info) {
        return new File(driversDir(context), info.id);
    }

    // -------------------------------------------------------------- listing

    /** Every importable driver on disk, newest-looking name order, never null. */
    public static List<DriverInfo> list(Context context) {
        List<DriverInfo> out = new ArrayList<>();
        File[] dirs = driversDir(context).listFiles();
        if (dirs == null) {
            return out;
        }
        for (File dir : dirs) {
            if (!dir.isDirectory()) {
                continue;
            }
            DriverInfo info = readRecord(dir);
            // A directory with no readable record is a half-finished import (we
            // write the record last). Drop it rather than showing a broken row.
            if (info != null && new File(dir, info.libraryName).isFile()) {
                out.add(info);
            }
        }
        Collections.sort(out, new Comparator<DriverInfo>() {
            @Override
            public int compare(DriverInfo a, DriverInfo b) {
                return a.name.compareToIgnoreCase(b.name);
            }
        });
        return out;
    }

    @Nullable
    public static DriverInfo find(Context context, @Nullable String id) {
        if (id == null || id.isEmpty()) {
            return null;
        }
        for (DriverInfo info : list(context)) {
            if (info.id.equals(id)) {
                return info;
            }
        }
        return null;
    }

    // ------------------------------------------------------ active selection

    /** The selected driver, or null for "use the system driver". */
    @Nullable
    public static DriverInfo active(Context context) {
        return find(context, readState(context, ACTIVE_FILE));
    }

    /** Select a driver, or pass null to go back to the system driver. */
    public static void setActive(Context context, @Nullable DriverInfo info) {
        if (info == null) {
            deleteState(context, ACTIVE_FILE);
        } else {
            writeState(context, ACTIVE_FILE, info.id);
        }
        // A deliberate change supersedes any past complaint about a driver, and
        // clears the stale reporting from whatever ran last.
        deleteState(context, DISABLED_NOTICE_FILE);
        deleteState(context, LAST_DEVICE_FILE);
        deleteState(context, LAST_STATUS_FILE);
    }

    public static void remove(Context context, DriverInfo info) {
        DriverInfo current = active(context);
        if (current != null && current.id.equals(info.id)) {
            setActive(context, null);
        }
        deleteRecursively(directoryFor(context, info));
    }

    // ----------------------------------------------------------- crash latch

    /**
     * Record that we are about to launch with {@code info}. Must be called
     * immediately before handing the driver to native code, and the write must be
     * on disk before the driver is touched — the process may not survive it.
     */
    public static void armBootLatch(Context context, DriverInfo info) {
        writeState(context, BOOT_PENDING_FILE, info.id);
    }

    /**
     * Declare this launch a success, so the driver is not deselected next time.
     *
     * <p>Called on a timer rather than when the renderer reports a working device,
     * and that is the whole point: the Adreno fault this feature exists for kills
     * the process about a second <em>after</em> rendering starts, so clearing on
     * renderer setup would disarm the latch immediately before the crash it is
     * meant to catch. Surviving a wall-clock window covers both that and a driver
     * that never initialises at all.
     */
    public static void clearBootLatch(Context context) {
        deleteState(context, BOOT_PENDING_FILE);
    }

    /**
     * Check for a previous launch that did not survive, and deselect the driver if
     * so. Returns the name of the driver that was turned off, or null when the
     * last launch was fine.
     *
     * <p>Call before arming a new latch, and before loading any driver — that
     * ordering is what stops a driver which took the process down from being
     * loaded again.
     */
    @Nullable
    public static String consumeFailedBoot(Context context) {
        String pendingId = readState(context, BOOT_PENDING_FILE);
        deleteState(context, BOOT_PENDING_FILE);
        if (pendingId == null || pendingId.isEmpty()) {
            return null;
        }
        DriverInfo info = find(context, pendingId);
        String name = (info != null) ? info.name : pendingId;
        Log.w(TAG, "custom driver: '" + name + "' did not reach a working renderer last launch; "
                + "reverting to the system driver");
        setActive(context, null);
        writeState(context, DISABLED_NOTICE_FILE, name);
        return name;
    }

    /** Name of a driver that was auto-disabled and not yet shown to the user. */
    @Nullable
    public static String pendingDisabledNotice(Context context) {
        String notice = readState(context, DISABLED_NOTICE_FILE);
        return (notice == null || notice.isEmpty()) ? null : notice;
    }

    public static void clearDisabledNotice(Context context) {
        deleteState(context, DISABLED_NOTICE_FILE);
    }

    // ------------------------------------------------------------- reporting

    /**
     * The GPU the renderer actually came up on, as reported by native code after
     * a successful setup. This is the authority on whether a custom driver is in
     * use — the loader returning a handle is not.
     */
    @Nullable
    public static String lastRenderDevice(Context context) {
        String device = readState(context, LAST_DEVICE_FILE);
        return (device == null || device.isEmpty()) ? null : device;
    }

    /** Loader outcome from the last launch; see the STATUS_ constants. */
    public static int lastLoaderStatus(Context context) {
        String raw = readState(context, LAST_STATUS_FILE);
        if (raw == null || raw.isEmpty()) {
            return STATUS_NOT_ATTEMPTED;
        }
        try {
            return Integer.parseInt(raw.trim());
        } catch (NumberFormatException e) {
            return STATUS_NOT_ATTEMPTED;
        }
    }

    // ---------------------------------------------------------------- import

    /**
     * Import a driver from a user-picked document. Accepts an {@code .adpkg}
     * (a zip of {@code meta.json} plus the library) or a bare {@code .so}.
     *
     * <p>Runs on a background thread — it copies tens of megabytes.
     */
    public static DriverInfo importFrom(Context context, Uri uri) throws ImportException {
        File staging = null;
        File target = null;
        try {
            staging = copyToStaging(context, uri);

            String displayName = queryDisplayName(context, uri);
            boolean isZip = looksLikeZip(staging);

            String id = allocateId(context, displayName);
            target = new File(driversDir(context), id);
            deleteRecursively(target);
            if (!target.mkdirs()) {
                throw new ImportException(context.getString(R.string.driver_import_failed_storage));
            }

            DriverInfo info = isZip
                    ? unpackPackage(context, staging, target, id)
                    : unpackBareLibrary(context, staging, target, id, displayName);

            // Written last: list() treats a directory without a readable record as
            // a half-finished import and skips it, so a crash mid-import leaves
            // nothing that looks installed.
            writeRecord(new File(target, RECORD_FILE), info);
            target = null;
            return info;
        } catch (IOException e) {
            Log.e(TAG, "custom driver: import failed", e);
            throw new ImportException(context.getString(R.string.driver_import_failed_read));
        } finally {
            if (staging != null) {
                //noinspection ResultOfMethodCallIgnored
                staging.delete();
            }
            // Only set when we bailed out partway through.
            if (target != null) {
                deleteRecursively(target);
            }
        }
    }

    private static File copyToStaging(Context context, Uri uri) throws IOException, ImportException {
        File staging = File.createTempFile("import", ".bin", context.getCacheDir());
        long written = 0;
        try (InputStream in = context.getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(staging)) {
            if (in == null) {
                throw new ImportException(context.getString(R.string.driver_import_failed_read));
            }
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                written += n;
                if (written > MAX_IMPORT_BYTES) {
                    throw new ImportException(context.getString(R.string.driver_import_failed_too_large));
                }
                out.write(buf, 0, n);
            }
        }
        return staging;
    }

    /** Unpack an .adpkg: meta.json names the library, so honour it. */
    private static DriverInfo unpackPackage(Context context, File staging, File target, String id)
            throws IOException, ImportException {
        try (ZipFile zip = new ZipFile(staging)) {
            ZipEntry metaEntry = findEntry(zip, "meta.json");
            if (metaEntry == null) {
                throw new ImportException(context.getString(R.string.driver_import_failed_no_meta));
            }

            JSONObject meta;
            try (InputStream in = zip.getInputStream(metaEntry)) {
                meta = new JSONObject(new String(readAll(in), StandardCharsets.UTF_8));
            } catch (org.json.JSONException e) {
                throw new ImportException(context.getString(R.string.driver_import_failed_bad_meta));
            }

            String libraryName = meta.optString("libraryName", "");
            if (libraryName.isEmpty()) {
                throw new ImportException(context.getString(R.string.driver_import_failed_bad_meta));
            }
            // Only ever the basename: a libraryName carrying path separators would
            // otherwise let a crafted package write outside the driver directory,
            // and libadrenotools appends it to the directory as a plain name anyway.
            libraryName = new File(libraryName).getName();

            ZipEntry libEntry = findEntry(zip, libraryName);
            if (libEntry == null) {
                throw new ImportException(
                        context.getString(R.string.driver_import_failed_missing_lib, libraryName));
            }

            File libFile = new File(target, libraryName);
            try (InputStream in = zip.getInputStream(libEntry)) {
                writeTo(in, libFile);
            }
            verifyIsAarch64Library(context, libFile);

            String name = meta.optString("name", "");
            if (name.isEmpty()) {
                name = meta.optString("packageVersion", libraryName);
            }
            return new DriverInfo(
                    id,
                    name,
                    libraryName,
                    meta.optString("driverVersion", ""),
                    meta.optString("vendor", ""),
                    meta.optInt("minApi", 0));
        } catch (java.util.zip.ZipException e) {
            throw new ImportException(context.getString(R.string.driver_import_failed_bad_zip));
        }
    }

    /**
     * Adopt a bare .so. The filename is kept as-is where it can be: the ELF's own
     * SONAME is what the dynamic linker records, and keeping the two consistent
     * avoids surprising it. We do not read the SONAME out of the file — the name a
     * user picked has been right in every case seen so far, and a mismatch does
     * not stop the driver loading.
     */
    private static DriverInfo unpackBareLibrary(Context context, File staging, File target,
                                                String id, String displayName)
            throws IOException, ImportException {
        String libraryName = new File(displayName).getName();
        if (!libraryName.endsWith(".so")) {
            libraryName = libraryName + ".so";
        }
        File libFile = new File(target, libraryName);
        try (InputStream in = new FileInputStream(staging)) {
            writeTo(in, libFile);
        }
        verifyIsAarch64Library(context, libFile);
        // No metadata to go on, so the file is all we can name it after and minApi
        // is unknown (0 = do not check).
        return new DriverInfo(id, libraryName, libraryName, "", "", 0);
    }

    /**
     * Reject anything that is not a 64-bit AArch64 shared object before it can
     * reach {@code dlopen}. A 32-bit or x86 driver is a plausible mistake — the
     * download pages carry several architectures — and catching it here turns a
     * black screen and a crash-latch cycle into a sentence at import time.
     *
     * <p>Header fields only. Verifying the symbols a driver imports against what
     * this device's libc exports would catch more (that is how a driver needing
     * {@code pthread_getaffinity_np} fails on Android 12), but a driver that fails
     * to load is already handled: the loader falls back to the system driver and
     * the crash latch deselects it.
     */
    private static void verifyIsAarch64Library(Context context, File file)
            throws IOException, ImportException {
        byte[] header = new byte[20];
        try (InputStream in = new FileInputStream(file)) {
            int read = 0;
            while (read < header.length) {
                int n = in.read(header, read, header.length - read);
                if (n < 0) {
                    break;
                }
                read += n;
            }
            if (read < header.length) {
                throw new ImportException(context.getString(R.string.driver_import_failed_not_elf));
            }
        }
        boolean magicOk = header[0] == 0x7F && header[1] == 'E' && header[2] == 'L' && header[3] == 'F';
        if (!magicOk) {
            throw new ImportException(context.getString(R.string.driver_import_failed_not_elf));
        }
        // e_ident[EI_CLASS]: 2 is ELFCLASS64. e_ident[EI_DATA]: 1 is little-endian.
        if (header[4] != 2 || header[5] != 1) {
            throw new ImportException(context.getString(R.string.driver_import_failed_not_arm64));
        }
        // e_machine, a little-endian half at offset 18. 183 (0xB7) is EM_AARCH64.
        int machine = (header[18] & 0xFF) | ((header[19] & 0xFF) << 8);
        if (machine != 183) {
            throw new ImportException(context.getString(R.string.driver_import_failed_not_arm64));
        }
    }

    @Nullable
    private static ZipEntry findEntry(ZipFile zip, String basename) {
        java.util.Enumeration<? extends ZipEntry> entries = zip.entries();
        while (entries.hasMoreElements()) {
            ZipEntry entry = entries.nextElement();
            if (entry.isDirectory()) {
                continue;
            }
            String name = entry.getName();
            int slash = name.lastIndexOf('/');
            String tail = (slash >= 0) ? name.substring(slash + 1) : name;
            if (tail.equalsIgnoreCase(basename)) {
                return entry;
            }
        }
        return null;
    }

    private static boolean looksLikeZip(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            byte[] magic = new byte[4];
            if (in.read(magic) < 4) {
                return false;
            }
            return magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4;
        }
    }

    // --------------------------------------------------------------- records

    @Nullable
    private static DriverInfo readRecord(File dir) {
        File file = new File(dir, RECORD_FILE);
        if (!file.isFile()) {
            return null;
        }
        try (InputStream in = new FileInputStream(file)) {
            JSONObject json = new JSONObject(new String(readAll(in), StandardCharsets.UTF_8));
            String libraryName = json.optString("libraryName", "");
            if (libraryName.isEmpty()) {
                return null;
            }
            return new DriverInfo(
                    dir.getName(),
                    json.optString("name", libraryName),
                    libraryName,
                    json.optString("driverVersion", ""),
                    json.optString("vendor", ""),
                    json.optInt("minApi", 0));
        } catch (IOException | org.json.JSONException e) {
            Log.w(TAG, "custom driver: unreadable record in " + dir.getName(), e);
            return null;
        }
    }

    private static void writeRecord(File file, DriverInfo info) throws IOException {
        JSONObject json = new JSONObject();
        try {
            json.put("name", info.name);
            json.put("libraryName", info.libraryName);
            json.put("driverVersion", info.driverVersion);
            json.put("vendor", info.vendor);
            json.put("minApi", info.minApi);
        } catch (org.json.JSONException e) {
            throw new IOException(e);
        }
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(json.toString().getBytes(StandardCharsets.UTF_8));
        }
    }

    /** A filesystem-safe directory name that does not collide with an existing one. */
    private static String allocateId(Context context, String displayName) {
        String base = displayName.replaceAll("[^A-Za-z0-9._-]", "_");
        if (base.endsWith(".adpkg.zip")) {
            base = base.substring(0, base.length() - ".adpkg.zip".length());
        } else if (base.endsWith(".zip") || base.endsWith(".so")) {
            base = base.substring(0, base.lastIndexOf('.'));
        }
        if (base.isEmpty()) {
            base = "driver";
        }
        if (base.length() > 48) {
            base = base.substring(0, 48);
        }
        File dir = driversDir(context);
        String candidate = base;
        int suffix = 2;
        while (new File(dir, candidate).exists()) {
            candidate = base + "-" + suffix;
            suffix++;
        }
        return candidate;
    }

    private static String queryDisplayName(Context context, Uri uri) {
        try (Cursor cursor = context.getContentResolver()
                .query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) {
                    String name = cursor.getString(column);
                    if (name != null && !name.isEmpty()) {
                        return name;
                    }
                }
            }
        } catch (RuntimeException e) {
            // Some providers refuse this query; the fallback below is fine.
            Log.w(TAG, "custom driver: no display name for " + uri, e);
        }
        String path = uri.getLastPathSegment();
        return (path == null || path.isEmpty()) ? "driver.so" : new File(path).getName();
    }

    // ----------------------------------------------------------- state files

    @Nullable
    private static String readState(Context context, String name) {
        File file = new File(stateDir(context), name);
        if (!file.isFile()) {
            return null;
        }
        try (InputStream in = new FileInputStream(file)) {
            return new String(readAll(in), StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return null;
        }
    }

    private static void writeState(Context context, String name, String value) {
        File dir = stateDir(context);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            Log.e(TAG, "custom driver: cannot create " + dir);
            return;
        }
        File file = new File(dir, name);
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(value.getBytes(StandardCharsets.UTF_8));
            // The boot latch is read after a process death that may follow within
            // milliseconds, so it has to be durable, not merely written.
            out.getFD().sync();
        } catch (IOException e) {
            Log.e(TAG, "custom driver: cannot write " + name, e);
        }
    }

    private static void deleteState(Context context, String name) {
        //noinspection ResultOfMethodCallIgnored
        new File(stateDir(context), name).delete();
    }

    /** Create every directory the loader and this class expect to exist. */
    public static void ensureDirectories(Context context) {
        //noinspection ResultOfMethodCallIgnored
        driversDir(context).mkdirs();
        //noinspection ResultOfMethodCallIgnored
        stateDir(context).mkdirs();
        //noinspection ResultOfMethodCallIgnored
        tmpDir(context).mkdirs();
    }

    // ------------------------------------------------------------------ util

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    private static void writeTo(InputStream in, File file) throws IOException {
        try (OutputStream out = new FileOutputStream(file)) {
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        }
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
