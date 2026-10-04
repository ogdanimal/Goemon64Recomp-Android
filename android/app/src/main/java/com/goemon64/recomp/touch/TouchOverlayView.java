package com.goemon64.recomp.touch;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.util.AttributeSet;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

import java.util.List;

/**
 * The on-screen N64 controls: a transparent {@link View} composited over SDL's
 * surface.
 *
 * <h2>Why a View and not the game's own renderer</h2>
 * The recomp already has an RmlUi layer, so drawing the pad there was the obvious
 * alternative. A plain Android View wins on every axis that matters here. It needs
 * no knowledge of RT64 or Vulkan, so the native build is untouched by changes to it,
 * and it gets Android's own multi-touch, haptics and safe-area insets for free. SDL's surface is
 * a plain {@code SurfaceView} with no {@code setZOrderOnTop}, so a sibling view added
 * after it composites cleanly on top.
 *
 * <p>Everything is drawn as vectors — no bitmaps. That keeps the APK unchanged in
 * size, stays sharp at any density from a phone to a handheld's 1080p panel, and
 * means restyling the pad is an edit to this file rather than a round trip through
 * an image pipeline.
 *
 * <h2>Redraw policy</h2>
 * The view invalidates on state change only, never per frame. An idle overlay costs
 * nothing; a pad being played costs one damage rect per touch event, on a surface
 * the compositor is already updating for the game.
 */
public class TouchOverlayView extends View {

    /** Idle and pressed alpha, as multipliers on the configured opacity. */
    private static final float ALPHA_IDLE = 1.0f;
    private static final float ALPHA_PRESSED = 1.55f;

    /**
     * How long the settings handle must be held to open the overlay's own settings.
     * A tap on it opens the game's menu, which is the common case; the long press is
     * the escape hatch to size, opacity and the layout editor.
     */
    private static final long LONG_PRESS_MS = 550L;

    /** How far a finger may drift during a long press before it stops counting. */
    private static final float LONG_PRESS_SLOP_PX = 24f;

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final RectF rect = new RectF();

    private TouchLayout layout = TouchLayout.defaults();
    private TouchPad pad = new TouchPad(layout);

    private Vibrator vibrator;

    /**
     * Set if the vibrator ever refuses. Haptics fire on every single button press, so
     * this path must be incapable of taking the app down: a device that throws once
     * will throw every time, and the correct outcome is silently no haptics, not a
     * crash mid-game. Belt and braces alongside the VIBRATE permission in the
     * manifest -- OEM vibrator implementations are a well-known source of surprises.
     */
    private boolean hapticsUnavailable;

    /**
     * True once a gamepad has been used, and seeded at startup from whether one is
     * already attached. In AUTO visibility this hides the overlay, and the next
     * screen touch brings it back — the same behaviour a player expects
     * from any mobile front-end, and the reason a handheld with real sticks never
     * has to visit a settings screen to get its screen back.
     */
    private boolean gamepadUsed;

    /** Set while the native UI is capturing input, so the pad hides for menus. */
    private boolean menuOpen;

    /** Editor mode: controls are dragged rather than pressed. */
    private boolean editing;
    private TouchControl dragging;
    /**
     * Last control touched in the editor. Kept after the drag ends so the resize
     * buttons have something to act on — you pick a control by touching it, then
     * size it, rather than having to hold it while reaching for a button.
     */
    private TouchControl selected;
    private float dragDx;
    private float dragDy;
    private Runnable onLayoutChanged;
    private Runnable onSettingsRequested;

    /**
     * Long-press bookkeeping for the settings handle.
     *
     * <p>The handle is the one control with click-on-release semantics rather than
     * press-on-touch. It has to be: a tap opens the game's menu and a long press
     * opens the overlay's own settings, and those are only distinguishable once the
     * finger lifts. Sending the press on touch-down and retracting it later does not
     * work — the game would already have seen a complete press and toggled its menu
     * open behind the settings dialog. Every other control still fires on contact,
     * because for a gameplay button that latency would be the whole ballgame.
     */
    private int menuPointerId = -1;
    private float menuDownX;
    private float menuDownY;
    /** Drawn as held while the finger is down, even though nothing has been sent. */
    private boolean menuVisualHeld;

    private float insetLeft;
    private float insetTop;
    private float insetRight;
    private float insetBottom;

    private final Handler handler = new Handler(Looper.getMainLooper());

    public TouchOverlayView(Context context) {
        super(context);
        init();
    }

    public TouchOverlayView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        setFocusable(false);
        setFocusableInTouchMode(false);
        // The game's surface is below; this view must never paint a background over it.
        setBackgroundColor(Color.TRANSPARENT);
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        vibrator = (Vibrator) getContext().getSystemService(Context.VIBRATOR_SERVICE);
        // Start hidden if a pad is already attached; see gamepadConnected().
        gamepadUsed = gamepadConnected();
        setLayout(layout);
    }

    // ------------------------------------------------------------------ wiring

    public void setLayout(TouchLayout layout) {
        this.layout = layout;
        pad.setLayout(layout);
        applyInsets();
        syncActive();
        invalidate();
    }

    public TouchLayout layout() {
        return layout;
    }

    public TouchPad pad() {
        return pad;
    }

    /** Called after the editor moves something, so the caller can persist it. */
    public void setOnLayoutChanged(Runnable listener) {
        this.onLayoutChanged = listener;
    }

    /** Called when the settings handle is long-pressed. */
    public void setOnSettingsRequested(Runnable listener) {
        this.onSettingsRequested = listener;
    }

    public void setEditing(boolean editing) {
        if (this.editing == editing) {
            return;
        }
        this.editing = editing;
        // Leaving any held button behind would be sent to the game for as long as the
        // editor is open.
        pad.clear();
        pushState();
        dragging = null;
        selected = null;
        invalidate();
    }

    public boolean isEditing() {
        return editing;
    }

    /**
     * Report that a physical gamepad was used. In AUTO visibility the overlay gets
     * out of the way until the screen is touched again.
     */
    public void noteGamepadUsed() {
        if (gamepadUsed) {
            return;
        }
        gamepadUsed = true;
        if (layout.visibility == TouchLayout.Visibility.AUTO) {
            pad.clear();
            pushState();
            invalidate();
            syncActive();
        }
    }

    /** Report that the native UI is (or is no longer) capturing input. */
    public void setMenuOpen(boolean menuOpen) {
        if (this.menuOpen == menuOpen) {
            return;
        }
        this.menuOpen = menuOpen;
        if (menuOpen) {
            pad.clear();
            pushState();
        }
        invalidate();
        syncActive();
    }

    /** Whether the pad should be drawn and should take touches right now. */
    public boolean isPadShown() {
        // Editing is checked BEFORE Off: the settings menu offers "Off" and "Edit
        // Layout" side by side, so a player can reach the editor with the overlay
        // turned off, and the editor has to show the controls it is editing.
        if (editing) {
            return true;
        }
        if (layout.visibility == TouchLayout.Visibility.NEVER) {
            return false;
        }
        // A menu hides the pad and, more importantly, stops it consuming touches, so
        // SDL's touch-to-mouse emulation can drive the RmlUi menu underneath.
        if (menuOpen) {
            return false;
        }
        if (layout.visibility == TouchLayout.Visibility.ALWAYS) {
            return true;
        }
        return !gamepadUsed;
    }

    private void syncActive() {
        NativeTouch.setActive(isPadShown());
    }

    private void pushState() {
        NativeTouch.setState(pad.buttonMask(), pad.axes());
    }

    /** Drop everything held. Called when the activity pauses or loses focus. */
    public void release() {
        cancelMenuLongPress();
        pad.clear();
        pushState();
        invalidate();
    }

    // ------------------------------------------------------------------ layout

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        applyInsets();
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        // Display cutouts and gesture bars: a button under either is a button that
        // cannot be pressed, so the whole layout is resolved inside the safe area.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && insets.getDisplayCutout() != null) {
            android.view.DisplayCutout cutout = insets.getDisplayCutout();
            insetLeft = cutout.getSafeInsetLeft();
            insetTop = cutout.getSafeInsetTop();
            insetRight = cutout.getSafeInsetRight();
            insetBottom = cutout.getSafeInsetBottom();
        } else {
            insetLeft = insetTop = insetRight = insetBottom = 0f;
        }
        applyInsets();
        // Returned unchanged, deliberately. This view is a sibling of SDL's surface in
        // the same layout, and a ViewGroup stops dispatching insets to later children
        // once one is consumed -- swallowing them here could leave the surface with no
        // cutout information at all. The overlay only wants to read them.
        return insets;
    }

    private void applyInsets() {
        pad.setSurface(getWidth(), getHeight(),
                insetLeft, insetTop, insetRight, insetBottom);
        invalidate();
    }

    // ------------------------------------------------------------------- input

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        // Same ordering as isPadShown(): with Off selected the editor still has to
        // take touches, or its controls draw but cannot be dragged.
        if (!editing && layout.visibility == TouchLayout.Visibility.NEVER) {
            return false;
        }

        // A screen touch means the player is back on the glass; undo the gamepad
        // auto-hide. Done before the shown check so the touch that brings the pad
        // back is not itself swallowed.
        if (gamepadUsed && !editing
                && event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            gamepadUsed = false;
            syncActive();
            invalidate();
        }

        if (!isPadShown()) {
            // Not ours: returning false lets the event fall through to SDL's surface,
            // which is what keeps the RmlUi menu touch-navigable.
            return false;
        }

        if (editing) {
            return handleEditTouch(event);
        }
        return handlePlayTouch(event);
    }

    /**
     * Drive the pad from a touch gesture.
     *
     * <p>Always returns true while the pad is shown, including for a touch that lands
     * on no control at all. That is not laziness — Android delivers every pointer of a
     * gesture to whichever view claimed its ACTION_DOWN. Letting an empty-space touch
     * fall through to SDL would hand SDL the whole gesture, and every later finger in
     * it, so a thumb resting on the picture would silently kill the buttons under the
     * other hand until it lifted. Owning the gesture costs nothing: while the pad is
     * shown there is no menu on screen for SDL's touch-to-mouse emulation to drive,
     * and the moment a menu opens the pad hides and stops consuming entirely.
     */
    private boolean handlePlayTouch(MotionEvent event) {
        int action = event.getActionMasked();

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int index = event.getActionIndex();
                int id = event.getPointerId(index);
                float x = event.getX(index);
                float y = event.getY(index);
                if (pad.controlAt(x, y) == TouchControl.MENU) {
                    // The handle NEVER reaches the pad -- it is routed through
                    // request_menu_toggle instead. Tested before the menuPointerId
                    // guard so that a second finger landing on the handle is dropped
                    // too, rather than falling through to pointerDown and latching
                    // TouchControl.MENU's SDL button (BACK) into the polled mask.
                    if (menuPointerId == -1) {
                        armLongPress(id, x, y);
                    }
                } else {
                    pad.pointerDown(id, x, y);
                }
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < event.getPointerCount(); i++) {
                    int id = event.getPointerId(i);
                    pad.pointerMove(id, event.getX(i), event.getY(i));
                    if (id == menuPointerId
                            && (Math.abs(event.getX(i) - menuDownX) > LONG_PRESS_SLOP_PX
                             || Math.abs(event.getY(i) - menuDownY) > LONG_PRESS_SLOP_PX)) {
                        cancelMenuLongPress();
                    }
                }
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int id = event.getPointerId(event.getActionIndex());
                if (id == menuPointerId) {
                    // Lifted before the long press completed, so it was a tap.
                    float x = menuDownX;
                    float y = menuDownY;
                    cancelMenuLongPress();
                    pulseMenu(x, y);
                }
                pad.pointerUp(id);
                break;
            }
            case MotionEvent.ACTION_CANCEL: {
                cancelMenuLongPress();
                pad.clear();
                break;
            }
            default:
                break;
        }

        if (pad.consumePressed()) {
            buzz();
        }
        pushState();
        invalidate();
        return true;
    }

    /**
     * Start counting a long press on the settings handle.
     *
     * <p>Nothing is sent to the game while the timer runs. If the finger lifts first
     * it was a tap, and the menu toggle is sent then ({@link #pulseMenu}); if the
     * timer completes, the overlay's own settings open instead and the game never
     * sees a press. See {@link #menuPointerId} for why the handle cannot fire on
     * contact the way every other control does.
     */
    private void armLongPress(int pointerId, float x, float y) {
        menuPointerId = pointerId;
        menuDownX = x;
        menuDownY = y;
        menuVisualHeld = true;
        handler.postDelayed(longPress, LONG_PRESS_MS);
    }

    private void cancelMenuLongPress() {
        if (menuPointerId != -1) {
            handler.removeCallbacks(longPress);
            menuPointerId = -1;
        }
        menuVisualHeld = false;
    }

    private final Runnable longPress = new Runnable() {
        @Override
        public void run() {
            if (menuPointerId == -1) {
                return;
            }
            menuPointerId = -1;
            menuVisualHeld = false;
            invalidate();
            if (onSettingsRequested == null) {
                return;
            }
            buzz();
            onSettingsRequested.run();
        }
    };

    /**
     * Send a tapped menu press as a short pulse, since the finger has already left
     * and there is no release left to drive it.
     */
    private void pulseMenu(float x, float y) {
        // Goes straight to the UI rather than through the virtual pad. The pad can only
        // set a bit in the polled button mask, and the native menu toggle is
        // event-driven, so a bit there would never be seen. requestMenuToggle queues a
        // real controller event on the same queue a physical pad feeds, which both
        // opens and closes the menu and follows a rebind of Toggle Menu.
        NativeTouch.requestMenuToggle();
    }

    private boolean handleEditTouch(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: {
                float x = event.getX();
                float y = event.getY();
                TouchLayout.Geometry hit = null;
                for (TouchLayout.Geometry g : pad.geometry()) {
                    if (g.hit(x, y, 1.15f)) {
                        hit = g;
                        break;
                    }
                }
                if (hit == null) {
                    // Still consumed: the editor is a modal state, and a stray tap must
                    // not reach the game underneath it.
                    return true;
                }
                // The C diamond moves as one piece, so a grab on any member drags the
                // anchor the other three are derived from.
                dragging = hit.control.isCButton() ? TouchControl.C_UP : hit.control;
                selected = dragging;
                if (onLayoutChanged != null) {
                    // Lets the toolbar relabel itself for the new selection.
                    onLayoutChanged.run();
                }
                TouchLayout.Geometry anchor = pad.geometryFor(dragging);
                dragDx = (anchor != null ? anchor.cx : x) - x;
                dragDy = (anchor != null ? anchor.cy : y) - y;
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                if (dragging == null) {
                    return true;
                }
                moveTo(dragging, event.getX() + dragDx, event.getY() + dragDy);
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL: {
                if (dragging != null) {
                    dragging = null;
                    if (onLayoutChanged != null) {
                        onLayoutChanged.run();
                    }
                    invalidate();
                }
                return true;
            }
            default:
                return true;
        }
    }

    /** The control the resize buttons act on, or null if nothing has been touched yet. */
    public TouchControl selected() {
        return selected;
    }

    /**
     * Grow or shrink the selected control.
     *
     * <p>Sizing the C cluster sizes all four of it. They move as one piece already
     * (their positions derive from C_UP's anchor), and resizing only the anchor would
     * change the diamond's spread as a side effect while leaving three buttons the old
     * size — visibly wrong, and not what "make this bigger" means.
     */
    public void nudgeSelectedScale(float delta) {
        if (selected == null) {
            return;
        }
        if (selected.isCButton()) {
            for (TouchControl c : TouchControl.C_CLUSTER) {
                applyScale(c, delta);
            }
        } else {
            applyScale(selected, delta);
        }
        pad.setLayout(layout);
        applyInsets();
        if (onLayoutChanged != null) {
            onLayoutChanged.run();
        }
    }

    private void applyScale(TouchControl control, float delta) {
        TouchLayout.Placement p = layout.placement(control);
        // Same bounds deserialize() enforces, so a value set here always round-trips.
        p.scale = TouchLayout.clamp(p.scale + delta, 0.5f, 2.0f);
    }

    /** Move a control to a pixel position, converting back to normalised storage. */
    private void moveTo(TouchControl control, float px, float py) {
        float availW = Math.max(1f, getWidth() - insetLeft - insetRight);
        float availH = Math.max(1f, getHeight() - insetTop - insetBottom);

        float nx = (px - insetLeft) / availW;
        float ny = (py - insetTop) / availH;

        TouchLayout.Placement p = layout.placement(control);
        p.x = TouchLayout.clamp01(nx);
        p.y = TouchLayout.clamp01(ny);
        pad.setLayout(layout);
        applyInsets();
    }

    /**
     * Whether a physical gamepad is attached right now.
     *
     * <p>Used once, to seed {@link #gamepadUsed} at startup. On a handheld with
     * built-in controls the overlay would otherwise be drawn over the game until the
     * first button press — a visible flash on a device that never wanted it.
     *
     * <p>This is deliberately NOT a device-type check. Android has no reliable "is
     * this a handheld" signal: {@code isExternal()} is hidden API, FEATURE_GAMEPAD is
     * reported inconsistently, and a model allowlist rots. Connected-at-startup is a
     * weaker signal, which is exactly why it only sets the <em>initial</em> value
     * rather than gating {@link #isPadShown()}: a pad paired but sitting in a drawer
     * costs the player one tap to get the overlay back, instead of leaving a phone
     * with no usable controls at all.
     */
    private static boolean gamepadConnected() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            // Device id 0 is the virtual keyboard, and some systems expose other
            // virtual sources; neither is a pad anyone is holding.
            if (device == null || device.isVirtual()) {
                continue;
            }
            int sources = device.getSources();
            if ((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
                return true;
            }
        }
        return false;
    }

    /**
     * Sniff a raw input event for gamepad use.
     *
     * <p>Called from the activity's dispatch hooks rather than being wired here,
     * because SDL owns the surface's own key and motion handling and this view must
     * not compete for it.
     */
    public static boolean isGamepadEvent(android.view.InputEvent event) {
        int source = event.getSource();
        boolean fromPad = (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
        if (!fromPad) {
            return false;
        }
        if (event instanceof KeyEvent) {
            // Volume and other system keys can arrive tagged with a pad source on some
            // devices; only a real button counts as "the player picked up a pad".
            int code = ((KeyEvent) event).getKeyCode();
            return KeyEvent.isGamepadButton(code)
                    || code == KeyEvent.KEYCODE_DPAD_UP
                    || code == KeyEvent.KEYCODE_DPAD_DOWN
                    || code == KeyEvent.KEYCODE_DPAD_LEFT
                    || code == KeyEvent.KEYCODE_DPAD_RIGHT;
        }
        return true;
    }

    private void buzz() {
        if (!layout.haptics || hapticsUnavailable) {
            return;
        }
        // A thumb on glass has no detent, so the buzz is the only confirmation a
        // press landed. Short and weak on purpose: this fires on every button.
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && vibrator != null
                    && vibrator.hasVibrator()) {
                vibrator.vibrate(VibrationEffect.createPredefined(VibrationEffect.EFFECT_TICK));
            } else {
                performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY,
                        HapticFeedbackConstants.FLAG_IGNORE_GLOBAL_SETTING);
            }
        } catch (RuntimeException e) {
            // Latched off rather than retried: whatever the cause (a missing
            // permission, an OEM vibrator service that rejects the effect), it will
            // not fix itself, and retrying would throw on every press for the rest
            // of the session.
            hapticsUnavailable = true;
        }
    }

    // ----------------------------------------------------------------- drawing

    @Override
    protected void onDraw(Canvas canvas) {
        if (!isPadShown()) {
            return;
        }
        List<TouchLayout.Geometry> geometry = pad.geometry();
        // Drawn back to front — largest first — which is the reverse of the hit-test
        // order, so the small precise controls are both on top and win a press.
        for (int i = geometry.size() - 1; i >= 0; i--) {
            draw(canvas, geometry.get(i));
        }
    }

    private int alphaFor(boolean held) {
        float a = layout.opacity * (held ? ALPHA_PRESSED : ALPHA_IDLE);
        return (int) (255 * TouchLayout.clamp(a, 0f, 1f));
    }

    private void draw(Canvas canvas, TouchLayout.Geometry g) {
        boolean held = pad.isHeld(g.control)
                || (g.control == TouchControl.MENU && menuVisualHeld);
        TouchControl marked = dragging != null ? dragging : selected;
        boolean grabbed = editing
                && (g.control == marked
                    || (marked == TouchControl.C_UP && g.control.isCButton()));
        int alpha = alphaFor(held || grabbed);

        // A dark translucent body under a light outline: the game's palette runs from
        // bright skies to near-black interiors, and an outline alone disappears
        // against half of it.
        fill.setColor(Color.BLACK);
        fill.setAlpha((int) (alpha * 0.45f));
        stroke.setColor(held || grabbed ? 0xFFFFFFFF : 0xFFDDE3EA);
        stroke.setAlpha(alpha);
        stroke.setStrokeWidth(Math.max(2f, g.rx * 0.055f));
        text.setColor(held || grabbed ? 0xFFFFFFFF : 0xFFDDE3EA);
        text.setAlpha(alpha);

        switch (g.control.kind) {
            case STICK:
                drawStick(canvas, g, alpha);
                break;
            case SHOULDER:
                rect.set(g.cx - g.rx, g.cy - g.ry, g.cx + g.rx, g.cy + g.ry);
                canvas.drawRoundRect(rect, g.ry, g.ry, fill);
                canvas.drawRoundRect(rect, g.ry, g.ry, stroke);
                drawLabel(canvas, g.control.label, g.cx, g.cy, g.ry * 0.95f);
                break;
            case MENU:
                canvas.drawCircle(g.cx, g.cy, g.rx, fill);
                canvas.drawCircle(g.cx, g.cy, g.rx, stroke);
                drawHamburger(canvas, g);
                break;
            default:
                canvas.drawCircle(g.cx, g.cy, g.rx, fill);
                canvas.drawCircle(g.cx, g.cy, g.rx, stroke);
                drawLabel(canvas, g.control.label, g.cx, g.cy,
                        g.rx * (g.control == TouchControl.START ? 0.42f : 0.78f));
                if (g.control.dir != TouchControl.Dir.NONE) {
                    drawArrow(canvas, g);
                }
                break;
        }
    }

    private void drawStick(Canvas canvas, TouchLayout.Geometry g, int alpha) {
        canvas.drawCircle(g.cx, g.cy, g.rx, fill);
        canvas.drawCircle(g.cx, g.cy, g.rx, stroke);

        // The knob shows the deflection actually being sent, which is the only
        // feedback a player has that the stick is reading what their thumb is doing.
        float knobR = g.rx * 0.42f;
        float travel = g.rx - knobR;
        float kx = g.cx + pad.stickX() * travel;
        float ky = g.cy + pad.stickY() * travel;

        fill.setColor(0xFFDDE3EA);
        fill.setAlpha((int) (alpha * 0.65f));
        canvas.drawCircle(kx, ky, knobR, fill);
        canvas.drawCircle(kx, ky, knobR, stroke);
    }

    private void drawLabel(Canvas canvas, String label, float cx, float cy, float size) {
        if (label == null || label.isEmpty()) {
            return;
        }
        text.setTextSize(size);
        // Centre on the glyphs' own vertical middle, not the baseline, so a label
        // sits optically centred in its button.
        Paint.FontMetrics fm = text.getFontMetrics();
        float baseline = cy - (fm.ascent + fm.descent) / 2f;
        canvas.drawText(label, cx, baseline, text);
    }

    /** The little direction arrow on a C button, drawn rather than typeset. */
    private void drawArrow(Canvas canvas, TouchLayout.Geometry g) {
        float r = g.rx * 0.30f;
        float offset = g.rx * 0.56f;
        float ax = g.cx;
        float ay = g.cy;
        switch (g.control.dir) {
            case UP:    ay -= offset; break;
            case DOWN:  ay += offset; break;
            case LEFT:  ax -= offset; break;
            default:    ax += offset; break;
        }
        path.reset();
        switch (g.control.dir) {
            case UP:
                path.moveTo(ax, ay - r); path.lineTo(ax + r, ay + r); path.lineTo(ax - r, ay + r);
                break;
            case DOWN:
                path.moveTo(ax, ay + r); path.lineTo(ax + r, ay - r); path.lineTo(ax - r, ay - r);
                break;
            case LEFT:
                path.moveTo(ax - r, ay); path.lineTo(ax + r, ay - r); path.lineTo(ax + r, ay + r);
                break;
            default:
                path.moveTo(ax + r, ay); path.lineTo(ax - r, ay - r); path.lineTo(ax - r, ay + r);
                break;
        }
        path.close();
        fill.setColor(text.getColor());
        fill.setAlpha(text.getAlpha());
        canvas.drawPath(path, fill);
    }

    private void drawHamburger(Canvas canvas, TouchLayout.Geometry g) {
        float w = g.rx * 0.52f;
        float gap = g.rx * 0.30f;
        stroke.setStrokeWidth(Math.max(2f, g.rx * 0.13f));
        for (int i = -1; i <= 1; i++) {
            canvas.drawLine(g.cx - w, g.cy + i * gap, g.cx + w, g.cy + i * gap, stroke);
        }
    }
}
