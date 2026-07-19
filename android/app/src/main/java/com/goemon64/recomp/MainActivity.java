package com.goemon64.recomp;

import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;

/**
 * Hosts the recompiled game. SDLActivity loads the native libraries listed in
 * getLibraries(), spins up its own thread, and calls SDL_main inside
 * libGoemon64.so. Before that we extract bundled assets to app-private storage
 * and hand the data directory to native code via nativeInit().
 */
public class MainActivity extends SDLActivity {

    /**
     * Intent extra: when true, native code calls start_game() during init so the
     * game boots straight to the title screen instead of stopping at the in-app
     * launcher. Set by the "Restart to Title Screen" menu option.
     */
    public static final String EXTRA_AUTOSTART = "com.goemon64.recomp.AUTOSTART";

    // Must match goemon64::RestartTarget in include/goemon_support.h.
    private static final int RESTART_NONE = 0;
    private static final int RESTART_APP_MENU = 1;
    private static final int RESTART_TITLE_SCREEN = 2;

    static {
        System.loadLibrary("SDL2");
        System.loadLibrary("Goemon64");
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "Goemon64" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Extract assets and set up the data dir BEFORE SDLActivity starts the
        // native thread (super.onCreate), since SDL_main reads them immediately.
        // Resolved via DataPaths so this agrees with LauncherActivity about
        // where data lives (internal or SD, chosen once at first run).
        //
        // Deliberately the never-null variant. We cannot bail out here: this is
        // SDLActivity, super.onCreate() loads the native libraries and starts
        // SDL_main, and returning before it throws SuperNotCalledException while
        // calling it would start the game with no data path. So the
        // "chosen volume is missing" guard lives in the only two callers that
        // can reach this activity (it is exported=false): LauncherActivity and
        // RestartActivity, both of which can refuse cheaply.
        File dataDir = DataPaths.dataDirOrInternal(this);
        if (!dataDir.exists()) {
            //noinspection ResultOfMethodCallIgnored
            dataDir.mkdirs();
        }
        AssetInstaller.installIfNeeded(this, dataDir);
        nativeInit(dataDir.getAbsolutePath(),
                getIntent().getBooleanExtra(EXTRA_AUTOSTART, false));

        super.onCreate(savedInstanceState);

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        hideSystemUI();
    }

    private void hideSystemUI() {
        View decorView = getWindow().getDecorView();
        int flags = View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
        decorView.setSystemUiVisibility(flags);
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUI();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        // Immersive flags set in onCreate()/onResume() run before the window
        // first gains focus, so Android drops them and the status/navigation
        // bars stay visible on launch. Re-apply once we actually have focus
        // (the documented Android requirement for sticky immersive mode).
        if (hasFocus) {
            hideSystemUI();
        }
    }

    @Override
    protected void onDestroy() {
        int restartTarget = nativeGetRestartTarget();
        nativeDestroy();
        super.onDestroy();

        if (restartTarget != RESTART_NONE) {
            restartInto(restartTarget == RESTART_TITLE_SCREEN);
            return;
        }

        // Plain quit. The native side latches `exited` for the lifetime of the
        // process, so if Android reuses this one for the next launch, game_main
        // returns immediately and the app comes up dead. Nothing is pending here
        // (unlike the restart path), so we can just take the process down.
        Runtime.getRuntime().halt(0);
    }

    /**
     * Relaunch the game in a fresh process.
     *
     * The native side latches a lot of state one-way for the lifetime of the
     * process ({@code exited}, {@code game_status}, the rdram allocation), so a
     * genuine cold boot means a new process rather than re-running the
     * entrypoint in place. We can't do that ourselves: killing this process
     * races the pending activity start, and starting an activity from a
     * finishing one is exactly the case Android's background-activity-start
     * rules exist to block. So hand off to RestartActivity, which lives in its
     * own process and can kill us from the outside — see that class.
     *
     * By this point the native shutdown has already flushed saves and torn down
     * the renderer, so being killed is safe.
     */
    private void restartInto(boolean autostart) {
        Intent intent = new Intent(this, RestartActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        intent.putExtra(EXTRA_AUTOSTART, autostart);
        intent.putExtra(RestartActivity.EXTRA_KILL_PID, android.os.Process.myPid());
        startActivity(intent);
    }

    // Implemented in android_glue.cpp.
    public native void nativeInit(String dataPath, boolean autostart);
    public native void nativeDestroy();
    public native int nativeGetRestartTarget();
}
