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

    private final ActivityResultLauncher<String[]> romPicker =
            registerForActivityResult(new ActivityResultContracts.OpenDocument(), this::onRomPicked);

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_launcher);

        missingRomView = findViewById(R.id.missingRomContainer);
        infoText = findViewById(R.id.infoText);
        pickRomButton = findViewById(R.id.pickRomButton);
        pickRomButton.setOnClickListener(v -> romPicker.launch(new String[]{"*/*"}));

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
            verifyAndMaybeStart(rom);
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
        try {
            copyRom(uri);
        } catch (IOException e) {
            Toast.makeText(this, "Failed to copy ROM: " + e.getMessage(), Toast.LENGTH_LONG).show();
            return;
        }
        File rom = romFile();
        if (rom != null && rom.exists() && rom.length() > 0) {
            verifyAndMaybeStart(rom);
        } else {
            Toast.makeText(this, "ROM copy failed", Toast.LENGTH_LONG).show();
        }
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

    private void verifyAndMaybeStart(File rom) {
        String sha1;
        try {
            sha1 = computeSha1(rom);
        } catch (Exception e) {
            // Couldn't read the file (or hash it) — that is NOT a checksum
            // mismatch, and must not route into the mismatch dialog whose primary
            // action deletes the ROM. A transient read error should never delete a
            // possibly-valid file.
            showRomUnreadableDialog(rom);
            return;
        }
        if (SHA1_NTSC_U.equalsIgnoreCase(sha1)) {
            startGame();
        } else {
            showHashMismatchDialog(rom, sha1);
        }
    }

    private void showRomUnreadableDialog(File rom) {
        new AlertDialog.Builder(this)
                .setTitle("Couldn't read the ROM")
                .setMessage("The ROM file couldn't be read to verify it. This is "
                        + "usually a temporary storage issue, not a bad ROM.\n\n"
                        + "Retry, or start anyway?")
                .setPositiveButton("Retry", (d, w) -> verifyAndMaybeStart(rom))
                .setNegativeButton("Start anyway", (d, w) -> startGame())
                .setCancelable(false)
                .show();
    }

    private void showAssetExtractionFailedDialog() {
        new AlertDialog.Builder(this)
                .setTitle("Setup failed")
                .setMessage("The app couldn't unpack its UI/asset files into "
                        + "storage, so the game can't start. This is usually a "
                        + "low-storage or permissions issue.\n\nFree up space and "
                        + "try again.")
                .setPositiveButton("Retry", (d, w) -> startGame())
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
                .setNegativeButton("Start anyway", (d, w) -> startGame())
                .setCancelable(false)
                .show();
    }

    private void startGame() {
        // Pre-flight asset extraction HERE, the launch chokepoint that can refuse.
        // MainActivity runs it too but cannot bail (SDLActivity's onCreate has
        // already committed to starting SDL_main), so a failure there launches the
        // native side against a partial asset tree and crashes opaquely. This
        // populates the same data dir MainActivity resolves, so its own call is a
        // no-op on success.
        //
        // Use the nullable dataDir() per DataPaths' convention — the OrInternal
        // variant is MainActivity's (it can't handle null). If the chosen volume
        // vanished between the storage choice and this button press, refuse like
        // the ROM path does rather than silently extracting to a fallback location.
        File dataDir = DataPaths.dataDir(this);
        if (dataDir == null) {
            showStorageUnavailableDialog();
            return;
        }
        if (!AssetInstaller.installIfNeeded(this, dataDir)) {
            showAssetExtractionFailedDialog();
            return;
        }
        Intent intent = new Intent(this, MainActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(intent);
        finish();
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
