package com.goemon64.recomp.touch;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Turns finger positions into a virtual gamepad state.
 *
 * <p>The whole class is plain Java — no {@code MotionEvent}, no {@code Canvas} — so
 * the input logic stays separate from Android's event and drawing types.
 * {@link TouchOverlayView} does nothing but unpack Android events into the four
 * methods below and hand the result to JNI.
 *
 * <h2>State, not edges</h2>
 * Each event recomputes the entire button mask from the set of live pointers,
 * rather than incrementing and decrementing a per-button hold count. Both approaches
 * handle two fingers on one button, but only this one is impossible to leave stuck:
 * there is no counter that can drift, so a pointer lost to a cancelled gesture or a
 * dropped UP event cannot strand a button held forever. Recomputing costs a loop
 * over at most a handful of pointers.
 */
public final class TouchPad {

    /**
     * How far past its drawn edge a control can be pressed. Thumbs are wide and
     * land low of where the player is aiming; see {@link TouchLayout.Geometry#hit}.
     */
    private static final float PRESS_SLOP = 1.25f;

    /**
     * How far a finger already holding a button may drift before it releases.
     * Larger than {@link #PRESS_SLOP} so that a thumb rolling on a button it is
     * deliberately holding does not chatter, while a deliberate slide off it still
     * lets go.
     */
    private static final float RELEASE_SLOP = 1.85f;

    /**
     * Dead centre of the analog stick. Deliberately tiny: the N64-accurate deadzone
     * that actually matters is applied natively in {@code controls.cpp}
     * ({@code inner_deadzone}), and stacking a second large one here would eat the
     * slow-walk range the game has. This exists only to keep a still thumb from
     * reporting a few thousandths of drift.
     */
    private static final float STICK_DEADZONE = 0.06f;

    /** What a single live pointer is currently doing. */
    private static final class Pointer {
        final TouchControl control;
        /** Stick only: current normalised offset from the base, in [-1, 1]. */
        float stickX;
        float stickY;
        /** Buttons only: cleared when the finger drifts past RELEASE_SLOP. */
        boolean holding = true;

        Pointer(TouchControl control) {
            this.control = control;
        }
    }

    private final Map<Integer, Pointer> pointers = new HashMap<>();

    /**
     * Stick response, 0..100, from the game's settings.
     *
     * <p>Turned into a gamma exponent by {@link #stickGamma()}: the thumb's distance
     * from the centre is raised to that power before being sent. 100 is linear.
     */
    private int stickSensitivity = 50;

    private TouchLayout layout;
    private float width;
    private float height;
    private float insetLeft;
    private float insetTop;
    private float insetRight;
    private float insetBottom;

    /** Cached resolved geometry, smallest control first — see {@link #controlAt}. */
    private List<TouchLayout.Geometry> resolved = new ArrayList<>();

    private int buttonMask;
    private final float[] axes = new float[TouchControl.Sdl.AXIS_COUNT];

    /** Set for one poll after the mask gains a bit, so the view can buzz. */
    private boolean pressedThisEvent;

    public TouchPad(TouchLayout layout) {
        this.layout = layout;
    }

    public void setLayout(TouchLayout layout) {
        this.layout = layout;
        resolve();
    }

    public TouchLayout layout() {
        return layout;
    }

    /** Stick response from the game config, 0..100. Clamped; out-of-range is ignored. */
    public void setStickSensitivity(int sensitivity) {
        this.stickSensitivity = Math.max(0, Math.min(100, sensitivity));
    }

    public int stickSensitivity() {
        return stickSensitivity;
    }

    /**
     * Exponent applied to the stick's magnitude.
     *
     * <p>1.0 (sensitivity 100) sends the thumb's distance from centre unchanged. That
     * is linear, and linear is the problem: the stick is roughly 9 mm across on a
     * 450 dpi phone, so the whole walking band sits inside about 4.5 mm of travel and
     * a slow walk cannot be held. Raising the magnitude to a power above 1 stretches
     * the low end — at 1.75, a thumb 30% out sends 13% rather than 30% — while 1.0 at
     * the rim is unchanged, so nothing is lost at the top.
     */
    private float stickGamma() {
        return 1.0f + (1.0f - stickSensitivity / 100.0f) * 1.5f;
    }

    public void setSurface(float width, float height,
                           float insetLeft, float insetTop,
                           float insetRight, float insetBottom) {
        this.width = width;
        this.height = height;
        this.insetLeft = insetLeft;
        this.insetTop = insetTop;
        this.insetRight = insetRight;
        this.insetBottom = insetBottom;
        resolve();
    }

    /**
     * Recompute pixel geometry, ordered smallest-first.
     *
     * <p>The order is the hit-test priority. Slop regions overlap — the stick's is
     * large and sits near B, the C diamond's members nearly touch — and without an
     * order the winner would depend on enum declaration order. Smallest-first means
     * the most precise target always wins, so a deliberate press on a small C button
     * is never swallowed by the stick's generous margin.
     */
    private void resolve() {
        List<TouchLayout.Geometry> list = new ArrayList<>();
        for (TouchControl c : TouchControl.values()) {
            if (layout.isVisible(c)) {
                list.add(layout.geometry(c, width, height,
                        insetLeft, insetTop, insetRight, insetBottom));
            }
        }
        list.sort((a, b) -> Float.compare(a.rx * a.ry, b.rx * b.ry));
        resolved = list;
    }

    /** Resolved geometry for drawing, in hit-test order. */
    public List<TouchLayout.Geometry> geometry() {
        return resolved;
    }

    public TouchLayout.Geometry geometryFor(TouchControl control) {
        for (TouchLayout.Geometry g : resolved) {
            if (g.control == control) {
                return g;
            }
        }
        return null;
    }

    /** Which control a point presses, or null. Exposed for the settings handle's
     *  click-on-release handling in {@link TouchOverlayView}. */
    public TouchControl controlAt(float x, float y) {
        TouchLayout.Geometry g = geometryAt(x, y);
        return g == null ? null : g.control;
    }

    private TouchLayout.Geometry geometryAt(float x, float y) {
        for (TouchLayout.Geometry g : resolved) {
            if (g.hit(x, y, PRESS_SLOP)) {
                return g;
            }
        }
        return null;
    }

    // ------------------------------------------------------------------- events

    /** @return true if this press landed on a control (and so should be consumed). */
    public boolean pointerDown(int pointerId, float x, float y) {
        TouchLayout.Geometry g = geometryAt(x, y);
        if (g == null) {
            return false;
        }
        Pointer p = new Pointer(g.control);
        pointers.put(pointerId, p);
        update(p, g, x, y);
        recompute();
        return true;
    }

    public void pointerMove(int pointerId, float x, float y) {
        Pointer p = pointers.get(pointerId);
        if (p == null) {
            return;
        }
        TouchLayout.Geometry g = geometryFor(p.control);
        if (g != null) {
            update(p, g, x, y);
        }
        recompute();
    }

    public void pointerUp(int pointerId) {
        if (pointers.remove(pointerId) != null) {
            recompute();
        }
    }

    /**
     * Drop every pointer. Called on ACTION_CANCEL and whenever the overlay stops
     * being shown — without it, a button held at the moment a gesture was stolen by
     * the system (a notification pull-down, a back swipe) would stay held forever.
     */
    public void clear() {
        if (!pointers.isEmpty()) {
            pointers.clear();
            recompute();
        }
    }

    /** Recompute one pointer's contribution from its current position. */
    private void update(Pointer p, TouchLayout.Geometry g, float x, float y) {
        switch (p.control.kind) {
            case STICK: {
                float dx = (x - g.cx) / g.rx;
                float dy = (y - g.cy) / g.ry;
                float len = (float) Math.sqrt(dx * dx + dy * dy);
                if (len < STICK_DEADZONE) {
                    p.stickX = 0f;
                    p.stickY = 0f;
                } else {
                    if (len > 1f) {
                        // Clamp to the rim but keep the direction, so the stick stays
                        // at full tilt while the finger travels outside the ring.
                        dx /= len;
                        dy /= len;
                        len = 1f;
                    }
                    // Shape the magnitude, not the components, so the direction the
                    // thumb is pointing is preserved exactly; only how far it counts
                    // as pushed changes. See stickGamma().
                    float shaped = (float) Math.pow(len, stickGamma());
                    float scale = shaped / len;
                    p.stickX = dx * scale;
                    p.stickY = dy * scale;
                }
                break;
            }
            default:
                p.holding = g.hit(x, y, RELEASE_SLOP);
                break;
        }
    }

    /** Rebuild the mask and axes from every live pointer. */
    private void recompute() {
        int previous = buttonMask;
        int mask = 0;
        Arrays.fill(axes, 0f);

        for (Pointer p : pointers.values()) {
            switch (p.control.kind) {
                case STICK:
                    // Two fingers on one stick is not a thing a hand does, but if it
                    // happens the larger deflection wins rather than the sum.
                    if (Math.abs(p.stickX) > Math.abs(axes[TouchControl.Sdl.AXIS_LEFTX])) {
                        axes[TouchControl.Sdl.AXIS_LEFTX] = p.stickX;
                    }
                    if (Math.abs(p.stickY) > Math.abs(axes[TouchControl.Sdl.AXIS_LEFTY])) {
                        axes[TouchControl.Sdl.AXIS_LEFTY] = p.stickY;
                    }
                    break;
                default:
                    if (!p.holding) {
                        break;
                    }
                    if (p.control.sdlButton != TouchControl.Sdl.NONE) {
                        mask |= 1 << p.control.sdlButton;
                    }
                    if (p.control.sdlAxis != TouchControl.Sdl.NONE) {
                        // Z and R are analog triggers in the stock mapping. A touch
                        // press is binary, so it reports a fully pulled trigger.
                        axes[p.control.sdlAxis] = 1.0f;
                    }
                    break;
            }
        }

        // Buzz on any 0 -> 1 transition, and only then: a second finger arriving on a
        // button that is already held must not retrigger it.
        // Accumulated, not assigned: recompute() runs once per pointer while
        // consumePressed() is read once per MotionEvent. Assigning would let the next
        // pointer's recompute (whose `previous` already holds the new bit) erase a real
        // 0->1 edge -- e.g. a finger sliding back onto a button while another finger is
        // moving would get no buzz. Cleared in consumePressed.
        pressedThisEvent |= (mask & ~previous) != 0;
        buttonMask = mask;
    }

    /** True if a control was newly pressed by the event just processed. */
    public boolean consumePressed() {
        boolean was = pressedThisEvent;
        pressedThisEvent = false;
        return was;
    }

    public int buttonMask() {
        return buttonMask;
    }

    public float[] axes() {
        return axes;
    }

    /** Whether a given control is currently held, for drawing it lit. */
    public boolean isHeld(TouchControl control) {
        for (Pointer p : pointers.values()) {
            if (p.control != control) {
                continue;
            }
            if (control.kind == TouchControl.Kind.STICK) {
                return true;
            }
            if (p.holding) {
                return true;
            }
        }
        // Otherwise lit if any finger is holding the same SDL button, so what is drawn
        // as held always matches what is actually being sent.
        if (control.sdlButton != TouchControl.Sdl.NONE) {
            return (buttonMask & (1 << control.sdlButton)) != 0;
        }
        return false;
    }

    /** Current stick deflection, for drawing the knob. */
    public float stickX() {
        return axes[TouchControl.Sdl.AXIS_LEFTX];
    }

    public float stickY() {
        return axes[TouchControl.Sdl.AXIS_LEFTY];
    }
}
