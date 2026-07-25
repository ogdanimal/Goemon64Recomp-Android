package com.goemon64.recomp;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;

import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Import and select a user-supplied Vulkan driver.
 *
 * <p>Exists only in a build made with {@code -PcustomDriver=true}; the manifest
 * entry is enabled by a placeholder driven from the same gradle flag, so a normal
 * build ships neither the icon nor a reachable activity. Without that gating a
 * user would find a driver screen that cannot do anything, because the native
 * loader is compiled out.
 *
 * <p>It is a separate launcher icon rather than a row in the in-game settings
 * menu, and that is the recovery path: a driver that fails to initialise takes
 * the game down before any in-game UI exists, so a setting inside the game would
 * be unreachable in precisely the situation it is needed for. The crash latch in
 * {@link GpuDriverStore} handles the automatic half; this screen is how a user
 * escapes a driver that starts fine and then renders wrong, which nothing can
 * detect for them.
 */
public class GpuDriverActivity extends AppCompatActivity {

    private LinearLayout driverList;
    private TextView statusText;

    private ExecutorService ioExecutor;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private volatile boolean destroyed = false;

    private final ActivityResultLauncher<String[]> driverPicker =
            registerForActivityResult(new ActivityResultContracts.OpenDocument(), this::onDriverPicked);

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_gpu_driver);

        driverList = findViewById(R.id.driverList);
        statusText = findViewById(R.id.driverStatusText);
        ioExecutor = Executors.newSingleThreadExecutor();

        GpuDriverStore.ensureDirectories(this);

        findViewById(R.id.importDriverButton).setOnClickListener(v -> pickDriver());
        findViewById(R.id.copyReportButton).setOnClickListener(v -> copyReport());

        // Arriving here is the user acknowledging whatever we auto-disabled; the
        // launcher has already had its chance to explain it.
        GpuDriverStore.clearDisabledNotice(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        if (ioExecutor != null) {
            ioExecutor.shutdownNow();
        }
        super.onDestroy();
    }

    // ------------------------------------------------------------------- UI

    private void refresh() {
        statusText.setText(buildStatusText());

        driverList.removeAllViews();
        GpuDriverStore.DriverInfo active = GpuDriverStore.active(this);

        addRow(getString(R.string.driver_system), getString(R.string.driver_system_subtitle),
                active == null, null);

        List<GpuDriverStore.DriverInfo> drivers = GpuDriverStore.list(this);
        for (GpuDriverStore.DriverInfo info : drivers) {
            String subtitle = info.subtitle();
            if (info.isTooNewForThisDevice()) {
                subtitle = getString(R.string.driver_min_api_warning, info.minApi, Build.VERSION.SDK_INT)
                        + "\n" + subtitle;
            }
            addRow(info.name, subtitle, active != null && active.id.equals(info.id), info);
        }

        if (drivers.isEmpty()) {
            TextView empty = new TextView(this);
            empty.setText(R.string.driver_none_imported);
            empty.setTextColor(0xB3FFFFFF);
            empty.setTextSize(13f);
            driverList.addView(empty);
        }
    }

    private void addRow(String title, String subtitle, boolean selected,
                        @Nullable GpuDriverStore.DriverInfo info) {
        View row = LayoutInflater.from(this).inflate(R.layout.item_gpu_driver, driverList, false);
        RadioButton radio = row.findViewById(R.id.driverRadio);
        Button remove = row.findViewById(R.id.driverRemoveButton);

        radio.setText(subtitle.isEmpty() ? title : (title + "\n" + subtitle));
        radio.setChecked(selected);
        radio.setOnClickListener(v -> select(info));

        if (info == null) {
            remove.setVisibility(View.GONE);
        } else {
            remove.setOnClickListener(v -> confirmRemove(info));
        }
        driverList.addView(row);
    }

    private void select(@Nullable GpuDriverStore.DriverInfo info) {
        GpuDriverStore.setActive(this, info);
        refresh();
        // The driver is chosen before the renderer starts, so nothing here can take
        // effect in a game that is already running.
        Toast.makeText(this, R.string.driver_restart_required, Toast.LENGTH_LONG).show();
    }

    private void confirmRemove(GpuDriverStore.DriverInfo info) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.driver_remove_title)
                .setMessage(getString(R.string.driver_remove_message, info.name))
                .setPositiveButton(R.string.driver_remove, (d, w) -> {
                    GpuDriverStore.remove(this, info);
                    refresh();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    // --------------------------------------------------------------- import

    private void pickDriver() {
        // Drivers arrive as .adpkg (a zip) or a bare .so, and neither has a MIME
        // type any provider agrees on, so the filter has to stay wide.
        try {
            driverPicker.launch(new String[] { "*/*" });
        } catch (RuntimeException e) {
            Toast.makeText(this, R.string.driver_import_failed_no_picker, Toast.LENGTH_LONG).show();
        }
    }

    private void onDriverPicked(@Nullable Uri uri) {
        if (uri == null) {
            return;
        }
        Toast.makeText(this, R.string.driver_importing, Toast.LENGTH_SHORT).show();
        ioExecutor.execute(() -> {
            try {
                GpuDriverStore.DriverInfo info = GpuDriverStore.importFrom(this, uri);
                postToUi(() -> onImported(info));
            } catch (GpuDriverStore.ImportException e) {
                String message = e.getMessage();
                postToUi(() -> showImportFailure(message));
            }
        });
    }

    private void onImported(GpuDriverStore.DriverInfo info) {
        refresh();
        // Importing is not selecting: silently switching the renderer underneath
        // someone who was only adding a file would make a crash-latch cycle their
        // introduction to the feature.
        new AlertDialog.Builder(this)
                .setTitle(R.string.driver_imported_title)
                .setMessage(getString(R.string.driver_imported_message, info.name))
                .setPositiveButton(R.string.driver_use_now, (d, w) -> select(info))
                .setNegativeButton(R.string.driver_keep_current, null)
                .show();
    }

    private void showImportFailure(@Nullable String message) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.driver_import_failed_title)
                .setMessage(message != null ? message : getString(R.string.driver_import_failed_read))
                .setPositiveButton(android.R.string.ok, null)
                .show();
    }

    // -------------------------------------------------------------- report

    /**
     * The text a bug report needs. Without it, "the game renders wrong" from a
     * user who may or may not be running a third-party driver is unanswerable —
     * and the device name is the only authority on which driver was actually
     * used, since the loader returning a handle proves nothing.
     */
    private String buildStatusText() {
        GpuDriverStore.DriverInfo active = GpuDriverStore.active(this);
        StringBuilder sb = new StringBuilder();

        sb.append(getString(R.string.driver_status_selected)).append(' ');
        sb.append(active == null ? getString(R.string.driver_system) : active.name).append('\n');

        String device = GpuDriverStore.lastRenderDevice(this);
        sb.append(getString(R.string.driver_status_gpu)).append(' ');
        sb.append(device != null ? device : getString(R.string.driver_status_unknown)).append('\n');

        sb.append(getString(R.string.driver_status_loader)).append(' ');
        sb.append(describeStatus(GpuDriverStore.lastLoaderStatus(this)));

        String notice = GpuDriverStore.pendingDisabledNotice(this);
        if (notice != null) {
            sb.append('\n').append(getString(R.string.driver_status_auto_disabled, notice));
        }
        return sb.toString();
    }

    private String describeStatus(int status) {
        switch (status) {
            case GpuDriverStore.STATUS_REQUESTED:
                return getString(R.string.driver_loader_requested);
            case GpuDriverStore.STATUS_OPEN_FAILED:
                return getString(R.string.driver_loader_open_failed);
            case GpuDriverStore.STATUS_NO_PROC_ADDR:
                return getString(R.string.driver_loader_no_proc_addr);
            case GpuDriverStore.STATUS_BAD_ARGUMENTS:
                return getString(R.string.driver_loader_bad_arguments);
            case GpuDriverStore.STATUS_NOT_ATTEMPTED:
            default:
                return getString(R.string.driver_loader_not_attempted);
        }
    }

    private void copyReport() {
        String report = getString(R.string.app_name) + " " + BuildConfig.VERSION_NAME + "\n"
                + "Device: " + Build.MANUFACTURER + " " + Build.MODEL
                + " (Android " + Build.VERSION.RELEASE + ", API " + Build.VERSION.SDK_INT + ")\n"
                + "ABI: " + (Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "unknown") + "\n"
                + buildStatusText();

        ClipboardManager clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard == null) {
            return;
        }
        clipboard.setPrimaryClip(ClipData.newPlainText(getString(R.string.driver_title), report));
        // Android 13+ shows its own clipboard confirmation; a toast there would be
        // a second one saying the same thing.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            Toast.makeText(this, R.string.driver_report_copied, Toast.LENGTH_SHORT).show();
        }
    }

    private void postToUi(Runnable r) {
        mainHandler.post(() -> {
            if (!destroyed && !isFinishing()) {
                r.run();
            }
        });
    }
}
