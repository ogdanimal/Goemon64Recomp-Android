package com.goemon64.recomp.touch;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.InputEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

/**
 * Owns the on-screen controls for one activity: installs the view over SDL's
 * surface, keeps it in step with the game's menu state, and puts up the settings
 * and layout editor.
 *
 * <p>Kept separate from {@link TouchOverlayView} so the view stays a view — drawing
 * and touch only — with the activity, dialogs and the polling loop kept out of it.
 */
public final class TouchOverlayController {

    /**
     * How often to ask native code whether a menu is up. Fast enough that the pad is
     * gone before a thumb arrives at the menu it just opened, slow enough to be
     * invisible on a battery graph — this is one atomic read per tick.
     */
    private static final long MENU_POLL_MS = 120L;

    private final Activity activity;
    private final TouchOverlayView view;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private boolean polling;

    /** SDL's layout, kept so the editor toolbar can be parented over the pad. */
    private ViewGroup host;

    private final Runnable menuPoll = new Runnable() {
        @Override
        public void run() {
            if (!polling) {
                return;
            }
            view.setMenuOpen(NativeTouch.isMenuOpen());
            // The visibility mode is owned by the game's settings menu, so it is read
            // on the same tick rather than pushed: the player can change it while the
            // overlay is on screen, and this way the pad follows within a frame or two
            // with no callback to wire up and nothing to keep in sync.
            TouchLayout.Visibility mode = NativeTouch.mode();
            if (mode != null && mode != view.layout().visibility) {
                TouchLayout layout = view.layout();
                layout.visibility = mode;
                view.setLayout(layout);
            }
            int sensitivity = NativeTouch.stickSensitivity();
            if (sensitivity >= 0 && sensitivity != view.pad().stickSensitivity()) {
                view.pad().setStickSensitivity(sensitivity);
            }
            handler.postDelayed(this, MENU_POLL_MS);
        }
    };

    /** Enter layout-edit mode. Called from the game's settings menu, off the UI thread. */
    public void requestEditorFromNative() {
        handler.post(() -> {
            if (host != null) {
                startEditing(host);
            }
        });
    }

    public TouchOverlayController(Activity activity) {
        this.activity = activity;
        this.view = new TouchOverlayView(activity);
        this.view.setLayout(TouchPrefs.load(activity));
        this.view.setOnLayoutChanged(this::persist);
    }

    public TouchOverlayView view() {
        return view;
    }

    /**
     * Add the overlay on top of an existing view hierarchy.
     *
     * <p>{@code parent} is SDL's own layout, which already holds its
     * {@code SurfaceView}. Adding afterwards puts this on top: the surface has no
     * {@code setZOrderOnTop}, so it composites below the window's ordinary views.
     */
    public void attachTo(ViewGroup parent) {
        host = parent;
        view.setOnSettingsRequested(() -> showSettings(parent));
        ViewGroup.LayoutParams lp = new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT);
        parent.addView(view, lp);
        // The surface below must keep receiving key events and SDL's own touches; the
        // overlay only ever consumes touches that land on a control.
        view.setFocusable(false);
    }

    public void onResume() {
        polling = true;
        handler.removeCallbacks(menuPoll);
        handler.post(menuPoll);
        NativeTouch.setActive(view.isPadShown());
    }

    public void onPause() {
        polling = false;
        handler.removeCallbacks(menuPoll);
        // Anything held when the app went away would otherwise still be held when it
        // comes back — or worse, for the rest of the session.
        view.release();
        NativeTouch.setActive(false);
    }

    /** Feed a raw input event in so a physical pad can auto-hide the overlay. */
    public void noteInputEvent(InputEvent event) {
        if (TouchOverlayView.isGamepadEvent(event)) {
            view.noteGamepadUsed();
        }
    }

    private void persist() {
        TouchPrefs.save(activity, view.layout());
    }

    // ------------------------------------------------------------- editor / UI

    /**
     * Put the pad into edit mode with a small toolbar, so controls can be dragged
     * where a given pair of hands actually wants them.
     *
     * <p>The editor is the overlay itself rather than a separate mock screen. The
     * only question it has to answer is "can my thumb reach that", and a mock at a
     * different size, on a different aspect ratio, without the game behind it,
     * cannot answer that question.
     */
    public void startEditing(ViewGroup parent) {
        if (view.isEditing()) {
            return;
        }
        view.setEditing(true);

        LinearLayout bar = new LinearLayout(activity);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER);
        bar.setBackgroundColor(0xCC000000);
        int pad = dp(10);
        bar.setPadding(pad, pad, pad, pad);

        final TextView hint = new TextView(activity);
        hint.setTextColor(0xFFFFFFFF);
        hint.setPadding(0, 0, dp(16), 0);
        bar.addView(hint);

        // Relabels as the selection changes, so the size buttons always say what they
        // are about to resize. Without it "-" and "+" are a guess.
        final Runnable relabel = () -> {
            TouchControl sel = view.selected();
            hint.setText(sel == null
                    ? "Drag to move  \u00b7  tap a control to size it"
                    : "Drag to move  \u00b7  sizing: " + label(sel));
        };
        relabel.run();

        Button smaller = new Button(activity);
        smaller.setText("\u2212");
        smaller.setOnClickListener(v -> {
            view.nudgeSelectedScale(-0.1f);
            relabel.run();
        });
        bar.addView(smaller);

        Button bigger = new Button(activity);
        bigger.setText("+");
        bigger.setOnClickListener(v -> {
            view.nudgeSelectedScale(0.1f);
            relabel.run();
        });
        bar.addView(bigger);

        // The view reports selection changes through the same callback it uses for
        // moves, so touching a control updates the label too.
        view.setOnLayoutChanged(() -> {
            persist();
            relabel.run();
        });

        Button reset = new Button(activity);
        reset.setText("Reset");
        reset.setOnClickListener(v -> {
            view.setLayout(TouchLayout.defaults());
            persist();
        });
        bar.addView(reset);

        Button done = new Button(activity);
        done.setText("Done");
        bar.addView(done);

        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;

        FrameLayout host = new FrameLayout(activity);
        host.addView(bar, lp);
        parent.addView(host, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        done.setOnClickListener(v -> {
            view.setEditing(false);
            persist();
            view.setOnLayoutChanged(this::persist);
            parent.removeView(host);
        });
    }

    /** The settings sheet: how big, how visible, and whether it buzzes. */
    public void showSettings(ViewGroup editorParent) {
        final ViewGroup parent = editorParent != null ? editorParent : host;
        TouchLayout layout = view.layout();

        LinearLayout root = new LinearLayout(activity);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        root.setPadding(pad, pad, pad, pad);

        // No show/hide control here on purpose. That setting lives in the game's own
        // menu (Settings -> Touch -> On-Screen Controls) and is stored in the game
        // config, which this sheet cannot write. The mode poll rewrites
        // layout.visibility from that config every 120 ms, so a duplicate control here
        // would be undone within a frame or two while still being persisted to
        // SharedPreferences -- two stores disagreeing behind a control that looks like
        // it works.
        TextView where = label("Show/hide is in the game menu: Settings \u2192 Touch");
        where.setTextColor(0xFF93A1B0);
        root.addView(where);

        root.addView(label("Size"));
        root.addView(slider(layout.globalScale, TouchLayout.SCALE_MIN, TouchLayout.SCALE_MAX,
                value -> {
                    layout.globalScale = value;
                    view.setLayout(layout);
                    persist();
                }));

        root.addView(label("Opacity"));
        root.addView(slider(layout.opacity, TouchLayout.OPACITY_MIN, TouchLayout.OPACITY_MAX,
                value -> {
                    layout.opacity = value;
                    view.setLayout(layout);
                    persist();
                }));

        android.widget.CheckBox haptics = new android.widget.CheckBox(activity);
        haptics.setText("Vibrate on press");
        haptics.setChecked(layout.haptics);
        haptics.setOnCheckedChangeListener((b, checked) -> {
            layout.haptics = checked;
            persist();
        });
        root.addView(haptics);

        new AlertDialog.Builder(activity)
                .setTitle("On-screen controls")
                .setView(root)
                .setPositiveButton("Close", null)
                .setNeutralButton("Edit layout…", (d, which) -> {
                    if (parent != null) {
                        startEditing(parent);
                    }
                })
                .show();
    }

    /** Human-readable name for the toolbar, since enum names are not player-facing. */
    private static String label(TouchControl control) {
        switch (control) {
            case STICK:   return "Stick";
            case START:   return "Start";
            case MENU:    return "Menu";
            case C_UP: case C_RIGHT: case C_DOWN: case C_LEFT:
                          return "C buttons";
            default:      return control.name();
        }
    }

    private TextView label(String s) {
        TextView t = new TextView(activity);
        t.setText(s);
        t.setPadding(0, dp(12), 0, 0);
        return t;
    }

    private interface OnValue {
        void accept(float value);
    }

    private SeekBar slider(float current, float min, float max, OnValue sink) {
        SeekBar bar = new SeekBar(activity);
        bar.setMax(100);
        bar.setProgress((int) ((current - min) / (max - min) * 100f));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar b, int progress, boolean fromUser) {
                if (fromUser) {
                    sink.accept(min + (max - min) * (progress / 100f));
                }
            }

            @Override
            public void onStartTrackingTouch(SeekBar b) {}

            @Override
            public void onStopTrackingTouch(SeekBar b) {}
        });
        return bar;
    }

    private int dp(int value) {
        return (int) (value * activity.getResources().getDisplayMetrics().density);
    }
}
