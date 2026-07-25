package com.goemon64.recomp;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

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

    // True while this process has a live game MainActivity. LauncherActivity (same
    // process) reads it to skip the ROM SHA-1 + storage/asset re-checks and resume
    // the running game on an icon relaunch, instead of re-running the whole
    // pipeline. A dead/killed process resets the static to false, so a genuine cold
    // launch still takes the full verify path. volatile: set on the main thread in
    // onCreate/onDestroy, read on the main thread in LauncherActivity.
    private static volatile boolean sGameRunning = false;

    public static boolean isGameRunning() {
        return sGameRunning;
    }

    // Set true after the first nativeInit in this process and NEVER reset — it
    // mirrors the native side's one-way latch (`exited`, `game_status`, the rdram
    // allocation). A SECOND MainActivity.onCreate in the SAME process (a
    // system-initiated recreation: a config change not in configChanges,
    // memory-pressure reclaim, or the "don't keep activities" dev option) cannot
    // re-init the latched runtime — the second nativeInit dies at the native
    // duplicate-symbol guard (a misleading "syms.ld" modal) or black-screens
    // because game_main returns immediately on the latched `exited`. onCreate
    // detects that and bounces to RestartActivity for a clean cold start instead.
    // singleTask routes an icon relaunch to onNewIntent (not onCreate), so the
    // fast-resume path never trips this. volatile: main-thread only, but paired
    // with sGameRunning for clarity.
    private static volatile boolean sNativeInitedThisProcess = false;

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
        // Same-process recreation recovery (S2). If this process already ran
        // nativeInit, the native runtime is latched one-way and a second init
        // here cannot work (see sNativeInitedThisProcess). Bounce to
        // RestartActivity — which kills this process from its own process and
        // cold-starts a clean one (or the launcher, if the SD volume is gone) —
        // rather than calling nativeInit again into a spent process. Safe despite
        // SDLActivity for the same reason as the storage guard below: super.
        // onCreate is mandatory, but this immediately-finishing activity never
        // reaches the RESUMED transition that starts SDL_main, and nativeInit is
        // skipped. In-game state is already lost to the recreation, so cold-start
        // to the app menu (autostart defaults to false in RestartActivity).
        if (sNativeInitedThisProcess) {
            super.onCreate(savedInstanceState);
            startActivity(new Intent(this, RestartActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK)
                    .putExtra(RestartActivity.EXTRA_KILL_PID, android.os.Process.myPid()));
            finish();
            return;
        }

        // Storage guard (M4). MainActivity is launchMode=singleTask, so recents can
        // recreate it DIRECTLY after a process death — a third entry point beyond
        // LauncherActivity/RestartActivity, which used to be the only ways in (both
        // guard the "chosen SD volume is missing" case). If the volume is gone,
        // bounce to LauncherActivity — which owns the "card missing" dialog —
        // instead of booting the game against a fallback internal path (which would
        // look, from the user's side, like the app ate their ROM and saves).
        //
        // Safe despite SDLActivity: super.onCreate() is mandatory here (skipping it
        // throws SuperNotCalledException), but SDLActivity starts SDL_main only on
        // the later RESUMED transition (surface-ready + focus + resumed), which this
        // immediately-finishing activity never reaches; and nativeInit is skipped,
        // so no game state initializes and g_data_dir stays empty.
        if (DataPaths.dataDir(this) == null) {
            super.onCreate(savedInstanceState);
            startActivity(new Intent(this, LauncherActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
            finish();
            return;
        }

        // Extract assets and set up the data dir BEFORE SDLActivity starts the
        // native thread (super.onCreate), since SDL_main reads them immediately.
        // Resolved via DataPaths so this agrees with LauncherActivity about where
        // data lives (internal or SD, chosen once at first run). The never-null
        // variant is safe now that the guard above has ruled out the missing-volume
        // case.
        File dataDir = DataPaths.dataDirOrInternal(this);
        if (!dataDir.exists()) {
            //noinspection ResultOfMethodCallIgnored
            dataDir.mkdirs();
        }
        AssetInstaller.installIfNeeded(this, dataDir);

        initCustomVulkanDriver();

        nativeInit(dataDir.getAbsolutePath(),
                getIntent().getBooleanExtra(EXTRA_AUTOSTART, false));

        // Latch that this process is now spent for re-init (see the field). Never
        // reset — a subsequent onCreate in this process means recreation, not a
        // fresh cold start, and must route through a new process.
        sNativeInitedThisProcess = true;

        // Committed to booting the game in this process now — let LauncherActivity
        // fast-resume us on an icon relaunch instead of re-verifying the ROM.
        sGameRunning = true;

        super.onCreate(savedInstanceState);

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        hideSystemUI();
    }

    /**
     * Hand the selected user-supplied Vulkan driver to native code, if there is
     * one and this build supports it.
     *
     * <p>Must run before {@code super.onCreate()}, which is the only window in
     * which a loader can be installed: SDLActivity starts the thread that
     * initialises the renderer, and by then volk has already resolved Vulkan.
     *
     * <p>The order here is the recovery path and matters. We consume any failed
     * boot FIRST, so a driver that took the process down last launch is
     * deselected before it can be loaded again — otherwise a driver that cannot
     * initialise is an unbreakable crash loop, because the game is what crashes
     * and the launcher goes straight back into it. Only then do we arm the latch
     * for this attempt.
     *
     * <p>The driver directory is deliberately {@code getFilesDir()} and NOT the
     * DataPaths data dir: {@code dlopen} refuses libraries on shared or SD
     * storage, and the data dir may well be a removable card.
     */
    private void initCustomVulkanDriver() {
        if (!BuildConfig.CUSTOM_VULKAN_DRIVER) {
            return;
        }
        GpuDriverStore.ensureDirectories(this);

        // Deselects the driver and leaves a notice for the launcher to show.
        GpuDriverStore.consumeFailedBoot(this);

        GpuDriverStore.DriverInfo driver = GpuDriverStore.active(this);
        String driverDir = "";
        String driverName = "";
        if (driver != null) {
            driverDir = GpuDriverStore.directoryFor(this, driver).getAbsolutePath();
            driverName = driver.libraryName;
            // Written and fsynced before the driver is touched, because the process
            // may not survive touching it. Nothing here disarms it: an unconfirmed
            // driver is disarmed only by the user answering the in-game prompt, and
            // a confirmed one by the renderer proving it is alive. Both of those
            // live on the game side; see GpuDriverStore's class comment.
            GpuDriverStore.armBootLatch(this, driver);
        }

        // nativeLibraryDir is where libadrenotools expects its own hook libraries,
        // and it only holds real files because the custom-driver build also sets
        // jniLibs.useLegacyPackaging.
        nativeInitCustomDriver(
                getApplicationInfo().nativeLibraryDir,
                driverDir,
                driverName,
                GpuDriverStore.tmpDir(this).getAbsolutePath(),
                GpuDriverStore.stateDir(this).getAbsolutePath());
    }

    // ------------------------------------------------- picking files for native
    //
    // Android has no native file dialog, so goemon64::open_file_dialog and the
    // GPU driver import both come back here: native asks, this launches the
    // system document picker, and the answer is handed back through a native
    // method once the document has been copied somewhere native code can open.
    //
    // These run on the game and render threads, so everything touching the
    // activity is posted to the UI thread, and everything touching the disk runs
    // on ioExecutor. SDLActivity extends android.app.Activity rather than
    // ComponentActivity, so this is the classic startActivityForResult pair and
    // not ActivityResultContracts.

    private static final int REQUEST_PICK_FILE = 0x60D0;
    private static final int REQUEST_PICK_FILES = 0x60D1;
    private static final int REQUEST_IMPORT_DRIVER = 0x60D2;

    /** UI-thread-only; created on first use because most sessions never pick a file. */
    private ExecutorService ioExecutor;

    /**
     * Ask the user for a file on behalf of native code. Returns immediately; the
     * result arrives at nativeOnFileDialogResult, possibly much later, and a
     * cancelled or failed pick reports failure rather than nothing so the caller's
     * callback is always run exactly once.
     */
    void requestOpenDocument(final boolean multiple) {
        runOnUiThread(() -> launchPicker(multiple ? REQUEST_PICK_FILES : REQUEST_PICK_FILE, multiple));
    }

    /** As above, but the file is imported as a GPU driver and never reaches native. */
    void requestDriverImport() {
        runOnUiThread(() -> launchPicker(REQUEST_IMPORT_DRIVER, false));
    }

    private void launchPicker(int requestCode, boolean multiple) {
        // Neither a mod nor a driver package has a MIME type providers agree on,
        // so the filter has to stay wide.
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*");
        if (multiple) {
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        }
        try {
            startActivityForResult(intent, requestCode);
        } catch (android.content.ActivityNotFoundException e) {
            android.util.Log.e("Goemon64", "no document picker available", e);
            deliverResult(requestCode, false, new String[0], getString(R.string.driver_import_failed_no_picker));
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_PICK_FILE && requestCode != REQUEST_PICK_FILES
                && requestCode != REQUEST_IMPORT_DRIVER) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }

        final List<Uri> uris = collectUris(resultCode, data);
        if (uris.isEmpty()) {
            // Cancelled. Reported as a failure with no message: the caller has to
            // hear something to stop waiting, but there is nothing to tell them.
            deliverResult(requestCode, false, new String[0], "");
            return;
        }

        if (ioExecutor == null) {
            ioExecutor = Executors.newSingleThreadExecutor();
        }
        ioExecutor.execute(() -> {
            if (requestCode == REQUEST_IMPORT_DRIVER) {
                importDriver(uris.get(0));
            } else {
                copyPickedFiles(requestCode, uris);
            }
        });
    }

    private List<Uri> collectUris(int resultCode, Intent data) {
        List<Uri> uris = new ArrayList<>();
        if (resultCode != RESULT_OK || data == null) {
            return uris;
        }
        android.content.ClipData clip = data.getClipData();
        if (clip != null) {
            for (int i = 0; i < clip.getItemCount(); i++) {
                Uri uri = clip.getItemAt(i).getUri();
                if (uri != null) {
                    uris.add(uri);
                }
            }
        } else if (data.getData() != null) {
            uris.add(data.getData());
        }
        return uris;
    }

    /** Background thread: copy each pick into the cache and hand back the paths. */
    private void copyPickedFiles(int requestCode, List<Uri> uris) {
        SafFiles.clearCache(this);
        List<String> paths = new ArrayList<>();
        for (int i = 0; i < uris.size(); i++) {
            try {
                paths.add(SafFiles.copyToCache(this, uris.get(i), i).getAbsolutePath());
            } catch (IOException e) {
                android.util.Log.e("Goemon64", "could not copy the picked file", e);
            }
        }
        // A partial result is still useful for a multi-select, but nothing copied
        // means the pick failed as far as the caller is concerned.
        deliverResult(requestCode, !paths.isEmpty(), paths.toArray(new String[0]), "");
    }

    /** Background thread: import a picked driver package into the driver store. */
    private void importDriver(Uri uri) {
        try {
            GpuDriverStore.DriverInfo info = GpuDriverStore.importFrom(this, uri);
            deliverResult(REQUEST_IMPORT_DRIVER, true, new String[0], info.name);
        } catch (GpuDriverStore.ImportException e) {
            String message = e.getMessage();
            deliverResult(REQUEST_IMPORT_DRIVER, false, new String[0],
                    message != null ? message : getString(R.string.driver_import_failed_read));
        }
    }

    private void deliverResult(int requestCode, boolean success, String[] paths, String message) {
        if (requestCode == REQUEST_IMPORT_DRIVER) {
            nativeOnDriverImportResult(success, message);
        } else {
            nativeOnFileDialogResult(success, paths);
        }
    }

    // ------------------------------------------------ GPU driver, for the game
    //
    // The driver settings live in the game's own RmlUi menu, so these are called
    // from the render thread. They are all small file operations on app-private
    // storage; GpuDriverStore stays the single authority on what is installed and
    // what is selected, rather than the native side reading the same files.

    String gpuDriverStateJson() {
        return GpuDriverStore.stateJson(this);
    }

    void gpuDriverSelect(String id) {
        GpuDriverStore.applySelection(this, id);
    }

    void gpuDriverRemove(String id) {
        GpuDriverStore.DriverInfo info = GpuDriverStore.find(this, id);
        if (info != null) {
            GpuDriverStore.remove(this, info);
        }
    }

    void gpuDriverConfirm(String id) {
        GpuDriverStore.confirmActive(this, id);
    }

    void gpuDriverNoteSurvived() {
        GpuDriverStore.noteActiveSurvived(this);
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
        // The game is going away (quit or restart-to-fresh-process); stop
        // advertising a live instance so a subsequent launch takes the full path.
        sGameRunning = false;

        if (BuildConfig.CUSTOM_VULKAN_DRIVER) {
            // Reaching onDestroy means the process was not taken down by the
            // driver, so this launch counts as survived — which matters for a
            // session shorter than the frame count the game side waits for. Only
            // has an effect for a driver the user already confirmed; an
            // unconfirmed one still needs the in-game answer, or quitting would
            // silently endorse a driver nobody ever saw draw anything.
            //
            // This is a BEST-EFFORT clear, not a guaranteed one, and the design
            // depends on it being the weaker of the two paths. Android only
            // promises onDestroy for a finishing activity: a swipe from recents
            // or a low-memory kill while backgrounded skips it. So a confirmed
            // driver can lose a launch that was fine — backgrounded at 10s, killed
            // by the system, and the next launch "recovers" from nothing, saying
            // the game did not start when it did.
            //
            // Accepted deliberately, because the ambiguity is two-sided and only
            // one side is fatal. "Backgrounded early, then killed" is also exactly
            // what a user does when fleeing a driver that hung, so clearing the
            // latch unconditionally in onStop would break the hang recovery this
            // whole mechanism exists for. A spurious revert is loud, self-
            // explaining and one selection away from undone; an un-recovered hang
            // is a crash loop with the setting that fixes it locked inside the
            // game that will not start. If it ever does bite, the discriminator is
            // already the design's own currency -- clear in onStop only once the
            // renderer has produced frames, since a hung driver stops producing
            // them.
            gpuDriverNoteSurvived();
        }
        if (ioExecutor != null) {
            ioExecutor.shutdownNow();
            ioExecutor = null;
        }
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
        //
        // But ONLY when the activity is genuinely finishing (user quit). onDestroy
        // also fires for system-initiated recreation — e.g. a config change not
        // listed in configChanges, or memory-pressure reclaim — and halting there
        // would kill the app out from under a legitimate recreation. isFinishing()
        // is exactly that distinction. (The configChanges list now absorbs the
        // common config-change recreations outright; and when a recreation does
        // slip through, the recreated onCreate detects the spent process via
        // sNativeInitedThisProcess and routes to a clean process restart — so this
        // branch correctly does nothing for the non-finishing case.)
        if (isFinishing()) {
            Runtime.getRuntime().halt(0);
        }
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
    /**
     * Loads a user-supplied Vulkan driver. No-op in a normal build, and no-op
     * when driverName is empty; returns a GpuDriverStore.STATUS_ value. See
     * android_glue.cpp.
     */
    public native int nativeInitCustomDriver(String hookLibDir, String driverDir, String driverName,
                                             String tmpLibDir, String stateDir);
    public native void nativeDestroy();
    public native int nativeGetRestartTarget();
    /**
     * Answers to a pick native code asked for. Called on a background thread; the
     * native side hands the result to the render thread itself. Paths are copies
     * in the cache, not the picked documents.
     */
    private native void nativeOnFileDialogResult(boolean success, String[] paths);
    /** Outcome of a driver import: the driver's name on success, else why not. */
    private native void nativeOnDriverImportResult(boolean success, String message);
}
