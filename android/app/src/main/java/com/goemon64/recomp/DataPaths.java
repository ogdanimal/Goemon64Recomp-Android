package com.goemon64.recomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Environment;

import androidx.annotation.Nullable;

import java.io.File;

/**
 * Single source of truth for where the app keeps its data dir (ROM, extracted
 * assets, configs and saves). Both LauncherActivity and MainActivity MUST go
 * through here — they previously each computed
 * {@code new File(getExternalFilesDir(null), "data")} independently, and two
 * copies of that decision is exactly how a storage choice drifts out of sync
 * and looks like the app ate the user's saves.
 *
 * <p>Two locations are supported:
 * <ul>
 *   <li>{@link #LOCATION_INTERNAL} — primary external files dir, which on
 *       essentially every modern device is emulated <em>internal</em> storage.
 *       This is the historical location and the default.</li>
 *   <li>{@link #LOCATION_SD} — the app-specific dir on a removable volume
 *       ({@code /storage/&lt;uuid&gt;/Android/data/&lt;pkg&gt;/files}). Needs no
 *       permissions and is removed on uninstall, same as internal.</li>
 * </ul>
 *
 * <p>The choice is stored as a plain flag rather than as a path, and the
 * removable volume is re-resolved on every call. Card paths are keyed by volume
 * UUID and can change across remounts, so a persisted absolute path would go
 * stale; re-resolving cannot.
 *
 * <p>The choice is made once, before any data exists, and is never migrated —
 * see the storage-choice UI in LauncherActivity. An install that already has
 * data is grandfathered to internal and never prompted, so existing users are
 * untouched by this feature.
 *
 * <p><b>DECIDED: the card being pulled MID-GAME is not handled, and that is
 * deliberate — do not "fix" it.</b> Availability is checked at launch only
 * (here, LauncherActivity and RestartActivity). Pulling the card while the game
 * runs is user error, and it is cheaper than it looks: with the volume gone,
 * writes fail outright rather than corrupting, so the files stay intact on the
 * card. The only real exposure is a removal landing inside the brief window of
 * an actual 128 KB save write, and the autosave feature's {@code .manual.bak}
 * rollback point already survives that.
 */
public final class DataPaths {
    public static final String LOCATION_INTERNAL = "internal";
    public static final String LOCATION_SD = "sd";

    private static final String PREFS = "storage";
    private static final String KEY_LOCATION = "data_location";
    private static final String DATA_DIR_NAME = "data";

    private DataPaths() {}

    private static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /** The stored choice, or null if the user has not been asked yet. */
    @Nullable
    public static String storedLocation(Context ctx) {
        return prefs(ctx).getString(KEY_LOCATION, null);
    }

    public static void setLocation(Context ctx, String location) {
        prefs(ctx).edit().putString(KEY_LOCATION, location).apply();
    }

    /**
     * The app-specific dir on a removable volume, or null if there is no
     * removable volume currently mounted.
     *
     * <p>{@code getExternalFilesDirs} returns the primary volume at index 0 and
     * any secondary volumes after it. Entries can be null when a volume is
     * mounted but not yet available, so every entry is null-checked before
     * {@link Environment#isExternalStorageRemovable(File)} is asked about it —
     * that call throws on a null argument.
     */
    @Nullable
    public static File removableDir(Context ctx) {
        File[] dirs = ctx.getExternalFilesDirs(null);
        if (dirs == null) {
            return null;
        }
        for (int i = 1; i < dirs.length; i++) {
            File dir = dirs[i];
            if (dir == null) {
                continue;
            }
            try {
                if (Environment.isExternalStorageRemovable(dir)) {
                    return dir;
                }
            } catch (Exception ignored) {
                // Defensive: some OEM builds throw for odd volume states. Skip.
            }
        }
        return null;
    }

    /** True if a removable volume is present, i.e. the SD option is offerable. */
    public static boolean hasRemovable(Context ctx) {
        return removableDir(ctx) != null;
    }

    /**
     * The data dir for the current choice, or null if the choice is SD and the
     * card is not currently available.
     *
     * <p>Returning null rather than silently falling back to internal is
     * deliberate. A silent fallback would create an empty data dir on internal
     * storage, re-extract assets into it, and present the user with a
     * first-run "pick your ROM" screen — indistinguishable, from their side,
     * from the app having deleted their ROM and saves. The caller is expected
     * to surface this as "card missing" and refuse to start.
     */
    @Nullable
    public static File dataDir(Context ctx) {
        if (LOCATION_SD.equals(storedLocation(ctx))) {
            File base = removableDir(ctx);
            return base == null ? null : new File(base, DATA_DIR_NAME);
        }
        return new File(ctx.getExternalFilesDir(null), DATA_DIR_NAME);
    }

    /** The internal data dir, regardless of the stored choice. */
    public static File internalDataDir(Context ctx) {
        return new File(ctx.getExternalFilesDir(null), DATA_DIR_NAME);
    }

    /**
     * Never-null variant, for the one caller that cannot tolerate null:
     * MainActivity is an SDLActivity, so it cannot return from onCreate before
     * super.onCreate() (SuperNotCalledException) and cannot call super without
     * a data path (SDL_main reads it immediately).
     *
     * <p>Do NOT use this anywhere a null could be handled properly. The
     * availability guard lives at every entry point to MainActivity:
     * LauncherActivity and RestartActivity (which can refuse cheaply), and — since
     * MainActivity became launchMode=singleTask (see AndroidManifest) so recents
     * can recreate it DIRECTLY after a process death — MainActivity.onCreate itself,
     * which bounces to LauncherActivity when {@link #dataDir} is null. By the time
     * this never-null variant runs, that guard has already ruled out the
     * missing-volume case. (SDL_main also aborts on an empty data dir as a native
     * backstop; see android_glue.cpp.)
     */
    public static File dataDirOrInternal(Context ctx) {
        File d = dataDir(ctx);
        return d != null ? d : internalDataDir(ctx);
    }

    /**
     * True if a usable install already exists at the internal location. Used to
     * grandfather existing installs: they keep working on internal and are
     * never shown the storage prompt.
     */
    public static boolean hasExistingInternalData(Context ctx) {
        File d = internalDataDir(ctx);
        if (!d.isDirectory()) {
            return false;
        }
        String[] entries = d.list();
        return entries != null && entries.length > 0;
    }

    /** Creates the data dir if needed. Returns false if it could not be created. */
    public static boolean ensureDataDir(Context ctx) {
        File d = dataDir(ctx);
        if (d == null) {
            return false;
        }
        if (!d.exists() && !d.mkdirs()) {
            return false;
        }
        return d.isDirectory();
    }
}
