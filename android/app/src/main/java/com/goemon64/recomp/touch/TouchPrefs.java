package com.goemon64.recomp.touch;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * Where the touch layout is persisted.
 *
 * <p>SharedPreferences rather than the game's own config: this is Android-side
 * presentation state that the native config system has no other reason to know
 * about, and keeping it here means the layout survives independently of the
 * recomp's config file (which is rewritten wholesale on a settings change) and can
 * be read by an activity that has not loaded the native library at all.
 */
public final class TouchPrefs {

    private static final String FILE = "touch_controls";
    private static final String KEY_LAYOUT = "layout";

    private TouchPrefs() {}

    private static SharedPreferences prefs(Context context) {
        return context.getApplicationContext()
                .getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    public static TouchLayout load(Context context) {
        return TouchLayout.deserialize(prefs(context).getString(KEY_LAYOUT, null));
    }

    public static void save(Context context, TouchLayout layout) {
        prefs(context).edit().putString(KEY_LAYOUT, layout.serialize()).apply();
    }
}
