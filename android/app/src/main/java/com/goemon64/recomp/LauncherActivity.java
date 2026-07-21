package com.goemon64.recomp;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.DigestInputStream;
import java.security.MessageDigest;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Ensures the Mystical Ninja Starring Goemon (USA) ROM is present in app-private
 * storage before launching SDL. The ROM is picked via SAF, copied to
 * <externalFilesDir>/data/mnsg.z64, and sha1-verified against the known NTSC-U
 * hash. Native code (android_glue.cpp nativeInit) registers it from that path.
 */
public class LauncherActivity extends AppCompatActivity {
    private static final String ROM_FILE_NAME = "mnsg.z64";
    // NTSC-U (USA) Mystical Ninja Starring Goemon, .z64 (big-endian).
    private static final String SHA1_NTSC_U = "df8083a54296b8c151917c5333e1c85f014a2a66";

    private View missingRomView;
    private TextView infoText;
    private Button pickRomButton;
    private View progressContainer;
    private TextView progressText;

    // Single background thread for the launch I/O (ROM copy, SHA-1, asset extract)
    // so it never blocks the UI thread. `destroyed` gates UI callbacks posted back
    // from that thread after the activity is gone.
    private ExecutorService ioExecutor;
    private volatile boolean destroyed = false;

    private final ActivityResultLauncher<String[]> romPicker =
            registerForActivityResult(new ActivityResultContracts.OpenDocument(), this::onRomPicked);

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Fast path: the game is already alive in this process (an icon relaunch of
        // a running game). Skip the ~16 MB ROM SHA-1 and the storage/asset re-checks
        // — they were all validated when the game booted — and just forward to the
        // running game task. MainActivity is singleTask, so this resolves to
        // onNewIntent on the live instance (an instant resume, no splash pause). A
        // dead process resets MainActivity.isGameRunning() to false, so a genuine
        // cold launch still falls through to the full verify path below.
        if (MainActivity.isGameRunning()) {
            android.util.Log.i("Goemon64", "LauncherActivity: game already running, fast-resume (skipping ROM verify)");
            Intent intent = new Intent(this, MainActivity.class);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(intent);
            finish();
            return;
        }

        setContentView(R.layout.activity_launcher);

        missingRomView = findViewById(R.id.missingRomContainer);
        infoText = findViewById(R.id.infoText);
        pickRomButton = findViewById(R.id.pickRomButton);
        pickRomButton.setOnClickListener(v -> romPicker.launch(new String[]{"*/*"}));
        progressContainer = findViewById(R.id.progressContainer);
        progressText = findViewById(R.id.progressText);
        ioExecutor = Executors.newSingleThreadExecutor();

        // Storage location is chosen ONCE, before any data exists. Existing
        // installs are grandfathered to internal and never prompted.
        if (DataPaths.storedLocation(this) == null) {
            if (DataPaths.hasExistingInternalData(this)) {
                DataPaths.setLocation(this, DataPaths.LOCATION_INTERNAL);
            } else if (DataPaths.hasRemovable(this)) {
                showStorageChoiceDialog();
                return;   // continues in proceedAfterStorageChoice()
            } else {
                DataPaths.setLocation(this, DataPaths.LOCATION_INTERNAL);
            }
        }

        proceedAfterStorageChoice();
    }

    /**
     * Offered only on a genuinely fresh install with a card present. There is no
     * migration path, so this is the one chance to choose — say so plainly rather
     * than implying it can be changed later.
     */
    private void showStorageChoiceDialog() {
        new AlertDialog.Builder(this)
                .setTitle("Where should game data go?")
                .setMessage("The ROM you pick, along with saves and UI files "
                        + "(about 45 MB), can live on internal storage or on your "
                        + "SD card.\n\nIf you choose the SD card, the game will not "
                        + "start while the card is removed.\n\nThis cannot be "
                        + "changed later without reinstalling.")
                .setPositiveButton("SD card", (d, w) -> {
                    DataPaths.setLocation(this, DataPaths.LOCATION_SD);
                    proceedAfterStorageChoice();
                })
                .setNegativeButton("Internal storage", (d, w) -> {
                    DataPaths.setLocation(this, DataPaths.LOCATION_INTERNAL);
                    proceedAfterStorageChoice();
                })
                .setCancelable(false)
                .show();
    }

    private void proceedAfterStorageChoice() {
        if (!DataPaths.ensureDataDir(this)) {
            showStorageUnavailableDialog();
            return;
        }

        File rom = romFile();
        if (rom != null && rom.exists() && rom.length() > 0) {
            prepareAndLaunch(rom);
        } else {
            showMissingRomUi();
        }
    }

    /**
     * The chosen volume is gone. Refuse rather than falling back to internal:
     * a fallback would silently present the first-run ROM picker, which from
     * the user's side is indistinguishable from the app having deleted their
     * ROM and saves.
     */
    private void showStorageUnavailableDialog() {
        new AlertDialog.Builder(this)
                .setTitle("SD card not available")
                .setMessage("This install keeps its game data on the SD card, "
                        + "which is not currently mounted.\n\nRe-insert the card "
                        + "and reopen the app. Your ROM and saves are on the card "
                        + "and have not been touched.")
                .setPositiveButton("Close", (d, w) -> finish())
                .setCancelable(false)
                .show();
    }

    @Nullable
    private File dataDir() {
        return DataPaths.dataDir(this);
    }

    /** Null when the chosen volume is unavailable — callers must handle it. */
    @Nullable
    private File romFile() {
        File d = dataDir();
        return d == null ? null : new File(d, ROM_FILE_NAME);
    }

    private void showMissingRomUi() {
        missingRomView.setVisibility(View.VISIBLE);
    }

    private void onRomPicked(@Nullable Uri uri) {
        if (uri == null) {
            Toast.makeText(this, "No file selected", Toast.LENGTH_SHORT).show();
            return;
        }
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {
            // Some providers don't support persistable permissions; not critical.
        }
        // Copy (16 MB) + verify + extract all run on the background thread now.
        runPreparePipeline(uri, romFile(), true);
    }

    private void copyRom(Uri source) throws IOException {
        if (!DataPaths.ensureDataDir(this)) {
            throw new IOException("Storage location unavailable");
        }
        File dest = romFile();
        if (dest == null) {
            throw new IOException("Storage location unavailable");
        }
        try (InputStream in = getContentResolver().openInputStream(source);
             FileOutputStream out = new FileOutputStream(dest)) {
            if (in == null) throw new IOException("Unable to open selected file");
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) != -1) {
                out.write(buf, 0, n);
            }
            out.flush();
        }
    }

    /** ROM present; SHA-1 verify + asset extract + launch, all off the UI thread. */
    private void prepareAndLaunch(File rom) {
        runPreparePipeline(null, rom, true);
    }

    /**
     * The single background launch pipeline. Runs the heavy launch I/O off the UI
     * thread (previously all synchronous → ANR on slow SD cards): optionally copy
     * the picked ROM, optionally SHA-1 verify it, extract the ~45 MB UI assets, and
     * launch. Status text, error dialogs, and the final launch are posted back to
     * the main thread; `verify` is false for the "Start anyway" / extract-retry
     * paths where the ROM has already been accepted.
     */
    private void runPreparePipeline(@Nullable Uri copyFrom, @Nullable File rom, boolean verify) {
        showProgress(copyFrom != null ? R.string.copying_rom : R.string.verifying_rom);
        ioExecutor.execute(() -> {
            // 1. Copy the picked ROM into private storage.
            if (copyFrom != null) {
                try {
                    copyRom(copyFrom);
                } catch (IOException e) {
                    postToUi(() -> {
                        hideProgress();
                        Toast.makeText(this, "Failed to copy ROM: " + e.getMessage(), Toast.LENGTH_LONG).show();
                        showMissingRomUi();
                    });
                    return;
                }
            }
            final File romFile = rom != null ? rom : romFile();
            if (romFile == null || !romFile.exists() || romFile.length() == 0) {
                postToUi(() -> {
                    hideProgress();
                    Toast.makeText(this, "ROM copy failed", Toast.LENGTH_LONG).show();
                    showMissingRomUi();
                });
                return;
            }

            // 2. SHA-1 verify (skipped on "Start anyway" / extract retry).
            if (verify) {
                String sha1;
                try {
                    sha1 = computeSha1(romFile);
                } catch (Exception e) {
                    // A read error is NOT a mismatch — route to the retry dialog, not
                    // the one whose primary action deletes the ROM.
                    postToUi(() -> { hideProgress(); showRomUnreadableDialog(romFile); });
                    return;
                }
                if (!SHA1_NTSC_U.equalsIgnoreCase(sha1)) {
                    final String got = sha1;
                    postToUi(() -> { hideProgress(); showHashMismatchDialog(romFile, got); });
                    return;
                }
            }

            // 3. Extract the UI assets. This is the launch chokepoint that can refuse:
            // MainActivity runs installIfNeeded too but cannot bail (SDLActivity's
            // onCreate commits to SDL_main), so pre-flighting here means a failure is
            // caught before the native side reads a partial asset tree. Populates the
            // same data dir MainActivity resolves, so its call is a no-op on success.
            setProgressText(R.string.preparing_files);
            File dataDir = DataPaths.dataDir(this);   // nullable variant: refuse if the volume vanished
            if (dataDir == null) {
                postToUi(() -> { hideProgress(); showStorageUnavailableDialog(); });
                return;
            }
            if (!AssetInstaller.installIfNeeded(this, dataDir)) {
                postToUi(() -> { hideProgress(); showAssetExtractionFailedDialog(romFile); });
                return;
            }

            // 4. Launch.
            postToUi(this::launchGame);
        });
    }

    private void launchGame() {
        Intent intent = new Intent(this, MainActivity.class);
        // NEW_TASK routes into MainActivity's own (distinct-affinity) task; if the
        // game is already alive, singleTask resolves it via onNewIntent. CLEAR_TOP
        // used to be here to force a clean top, but under singleTask it is inert —
        // and on the old standard launchMode it was exactly what destroyed and
        // recreated the running game on an icon relaunch. Dropped.
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(intent);
        finish();
    }

    // ---- progress UI + main-thread posting ----

    private void showProgress(int textRes) {
        missingRomView.setVisibility(View.GONE);
        progressText.setText(textRes);
        progressContainer.setVisibility(View.VISIBLE);
    }

    private void hideProgress() {
        progressContainer.setVisibility(View.GONE);
    }

    private void setProgressText(int textRes) {
        postToUi(() -> progressText.setText(textRes));
    }

    /** Run r on the UI thread, unless this activity has gone away. */
    private void postToUi(Runnable r) {
        if (destroyed) {
            return;
        }
        runOnUiThread(() -> {
            if (!destroyed && !isFinishing()) {
                r.run();
            }
        });
    }

    private void showRomUnreadableDialog(File rom) {
        new AlertDialog.Builder(this)
                .setTitle("Couldn't read the ROM")
                .setMessage("The ROM file couldn't be read to verify it. This is "
                        + "usually a temporary storage issue, not a bad ROM.\n\n"
                        + "Retry, or start anyway?")
                .setPositiveButton("Retry", (d, w) -> prepareAndLaunch(rom))
                .setNegativeButton("Start anyway", (d, w) -> runPreparePipeline(null, rom, false))
                .setCancelable(false)
                .show();
    }

    private void showAssetExtractionFailedDialog(File rom) {
        new AlertDialog.Builder(this)
                .setTitle("Setup failed")
                .setMessage("The app couldn't unpack its UI/asset files into "
                        + "storage, so the game can't start. This is usually a "
                        + "low-storage or permissions issue.\n\nFree up space and "
                        + "try again.")
                .setPositiveButton("Retry", (d, w) -> runPreparePipeline(null, rom, false))
                .setNegativeButton("Close", (d, w) -> finish())
                .setCancelable(false)
                .show();
    }

    private void showHashMismatchDialog(File rom, String got) {
        new AlertDialog.Builder(this)
                .setTitle("Unexpected ROM")
                .setMessage("Expected NTSC-U ROM\nsha1: " + SHA1_NTSC_U + "\n\nGot: " + got
                        + "\n\nStart anyway or pick a different .z64?")
                .setPositiveButton("Pick another", (d, w) -> {
                    //noinspection ResultOfMethodCallIgnored
                    rom.delete();
                    showMissingRomUi();
                })
                .setNegativeButton("Start anyway", (d, w) -> runPreparePipeline(null, rom, false))
                .setCancelable(false)
                .show();
    }

    @Override
    protected void onDestroy() {
        // Stop posting UI work from the background thread and interrupt any in-flight
        // copy/extract. A partial extract just leaves the version stamp unwritten, so
        // the next launch retries — the same failure-retry contract installIfNeeded
        // already relies on.
        destroyed = true;
        if (ioExecutor != null) {
            ioExecutor.shutdownNow();
        }
        super.onDestroy();
    }

    private String computeSha1(File file) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-1");
        byte[] buf = new byte[8192];
        try (InputStream in = new FileInputStream(file);
             DigestInputStream din = new DigestInputStream(in, md)) {
            //noinspection StatementWithEmptyBody
            while (din.read(buf) != -1) { /* digest updated via stream */ }
        }
        StringBuilder sb = new StringBuilder(40);
        for (byte b : md.digest()) {
            sb.append(String.format("%02x", b));
        }
        return sb.toString();
    }
}
