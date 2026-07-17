package com.goemon64.recomp;

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
        File dataDir = new File(getExternalFilesDir(null), "data");
        if (!dataDir.exists()) {
            //noinspection ResultOfMethodCallIgnored
            dataDir.mkdirs();
        }
        AssetInstaller.installIfNeeded(this, dataDir);
        nativeInit(dataDir.getAbsolutePath());

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
    protected void onDestroy() {
        nativeDestroy();
        super.onDestroy();
    }

    // Implemented in android_glue.cpp.
    public native void nativeInit(String dataPath);
    public native void nativeDestroy();
}
