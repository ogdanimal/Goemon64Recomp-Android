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

        ensureDataDir();

        File rom = romFile();
        if (rom.exists() && rom.length() > 0) {
            verifyAndMaybeStart(rom);
        } else {
            showMissingRomUi();
        }
    }

    private File dataDir() {
        return new File(getExternalFilesDir(null), "data");
    }

    private File romFile() {
        return new File(dataDir(), ROM_FILE_NAME);
    }

    private void ensureDataDir() {
        File d = dataDir();
        if (!d.exists()) {
            //noinspection ResultOfMethodCallIgnored
            d.mkdirs();
        }
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
        if (rom.exists() && rom.length() > 0) {
            verifyAndMaybeStart(rom);
        } else {
            Toast.makeText(this, "ROM copy failed", Toast.LENGTH_LONG).show();
        }
    }

    private void copyRom(Uri source) throws IOException {
        ensureDataDir();
        try (InputStream in = getContentResolver().openInputStream(source);
             FileOutputStream out = new FileOutputStream(romFile())) {
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
            sha1 = "<error>";
        }
        if (SHA1_NTSC_U.equalsIgnoreCase(sha1)) {
            startGame();
        } else {
            showHashMismatchDialog(rom, sha1);
        }
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
