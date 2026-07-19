package com.goemon64.recomp;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;

/**
 * Trampoline that relaunches the game in a genuinely fresh process.
 *
 * Restarting can't be done in-process: librecomp latches {@code exited},
 * {@code game_status} and the rdram allocation one-way for the lifetime of the
 * process, so a cold boot means a new process. That leaves the chicken-and-egg
 * problem of who starts the replacement — MainActivity can't, because by the
 * time its native shutdown is done it is finishing, and killing itself races
 * the pending activity start.
 *
 * So this activity runs in its own process (see android:process in the
 * manifest). MainActivity starts it while still foreground, it kills the game
 * process from the outside, then starts MainActivity again. Because it is
 * itself the visible activity at that point, the start is not subject to
 * background-activity-start restrictions.
 */
public class RestartActivity extends Activity {

    private static final String TAG = "Goemon64";

    public static final String EXTRA_KILL_PID = "com.goemon64.recomp.KILL_PID";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        boolean autostart = getIntent().getBooleanExtra(MainActivity.EXTRA_AUTOSTART, false);
        int killPid = getIntent().getIntExtra(EXTRA_KILL_PID, -1);

        // We kill whatever PID we were handed. That is safe enough to be worth
        // the simplicity: the activity is not exported, so only this app can
        // send the intent, and the PID comes straight from the caller's own
        // Process.myPid() microseconds earlier. A same-UID PID reuse inside that
        // window would be needed to hit the wrong process, and killProcess only
        // has permission over our own UID anyway.
        if (killPid > 0 && killPid != Process.myPid()) {
            Log.i(TAG, "RestartActivity: killing game process " + killPid);
            Process.killProcess(killPid);
        }

        // If the data lives on an SD card that has since been removed, relaunching
        // straight into MainActivity would start SDL against a path that no longer
        // exists. Route to the launcher instead: it owns the "card missing" dialog
        // and is the correct place to refuse. This runs on every restart, so it
        // also covers a card pulled while the game was running.
        Intent intent;
        if (DataPaths.dataDir(this) == null) {
            Log.i(TAG, "RestartActivity: chosen storage unavailable, routing to launcher");
            intent = new Intent(this, LauncherActivity.class);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        } else {
            intent = new Intent(this, MainActivity.class);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            intent.putExtra(MainActivity.EXTRA_AUTOSTART, autostart);
            Log.i(TAG, "RestartActivity: relaunching, autostart=" + autostart);
        }
        startActivity(intent);

        finish();
    }
}
