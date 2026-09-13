package com.goemon64.recomp.touch;

/**
 * The on-screen controls and what each one presses.
 *
 * <p>Every control emits an <em>SDL game controller</em> input, not an N64 button.
 * That indirection is the whole design: the overlay is merged into
 * {@code controller_button_state()} / {@code controller_axis_state()} alongside
 * physical pads (see {@code include/goemon_touch.h}), so whatever the player has
 * bound in Settings &rarr; Controls is what the on-screen button does. Rebinding B
 * moves the on-screen B with it, and a mod that reads L gets the on-screen L for
 * free. Emitting N64 buttons directly would instead have needed a second, parallel
 * binding system that could silently drift out of step with the real one.
 *
 * <p><b>Why the C-buttons emit D-pad.</b> The stock mapping gives each C direction
 * three bindings: a face/shoulder button, the right stick, and the D-pad. The
 * D-pad is the only one of the three that covers all four C directions (C-Right has
 * no face button at all) <em>and</em> survives analog-camera mode, which suppresses
 * the right stick. So D-pad is the binding that makes the on-screen C cluster
 * behave the same whether or not the analog camera is on.
 */
public enum TouchControl {

    /** Analog stick. Drives an axis pair rather than a button. */
    STICK(Kind.STICK, "", Dir.NONE, Sdl.NONE, Sdl.NONE),

    /** N64 A — jump. The big one on a real pad, so it gets the largest default radius. */
    A(Kind.BUTTON, "A", Dir.NONE, Sdl.BUTTON_A, Sdl.NONE),
    /** N64 B — attack. */
    B(Kind.BUTTON, "B", Dir.NONE, Sdl.BUTTON_B, Sdl.NONE),

    C_UP(Kind.BUTTON, "C", Dir.UP, Sdl.BUTTON_DPAD_UP, Sdl.NONE),
    C_RIGHT(Kind.BUTTON, "C", Dir.RIGHT, Sdl.BUTTON_DPAD_RIGHT, Sdl.NONE),
    C_DOWN(Kind.BUTTON, "C", Dir.DOWN, Sdl.BUTTON_DPAD_DOWN, Sdl.NONE),
    C_LEFT(Kind.BUTTON, "C", Dir.LEFT, Sdl.BUTTON_DPAD_LEFT, Sdl.NONE),

    /** N64 Z — crouch. An analog trigger in the stock mapping, so it drives an axis. */
    Z(Kind.SHOULDER, "Z", Dir.NONE, Sdl.NONE, Sdl.AXIS_TRIGGERLEFT),
    /** N64 L — unbound by this game; kept because mods use it. */
    L(Kind.SHOULDER, "L", Dir.NONE, Sdl.BUTTON_LEFTSHOULDER, Sdl.NONE),
    /** N64 R — camera / Hook Chain. Also an analog trigger in the stock mapping. */
    R(Kind.SHOULDER, "R", Dir.NONE, Sdl.NONE, Sdl.AXIS_TRIGGERRIGHT),

    START(Kind.BUTTON, "START", Dir.NONE, Sdl.BUTTON_START, Sdl.NONE),

    /**
     * Opens the recomp's own settings menu — the on-screen stand-in for Select.
     * Without it, a device with no gamepad could never reach Settings, because
     * Select appears nowhere else on the overlay.
     */
    MENU(Kind.MENU, "", Dir.NONE, Sdl.BUTTON_BACK, Sdl.NONE);

    /**
     * SDL input ids, mirrored from {@code SDL_gamecontroller.h}.
     *
     * <p>These live in a nested class for two reasons. Java forbids an enum
     * constant's arguments from referring to a static field of the same enum by
     * simple name (the fields initialise after the constants), so they could not
     * be plain fields here. And {@code src/main/android_touch.cpp} static-asserts
     * every value below against the real SDL enum, so an SDL renumbering breaks
     * the native build and names this file — instead of shipping an overlay where
     * every button silently presses the wrong thing.
     */
    public static final class Sdl {
        private Sdl() {}

        public static final int NONE = -1;

        // SDL_GameControllerButton
        public static final int BUTTON_A = 0;
        public static final int BUTTON_B = 1;
        public static final int BUTTON_BACK = 4;
        public static final int BUTTON_START = 6;
        public static final int BUTTON_LEFTSHOULDER = 9;
        public static final int BUTTON_DPAD_UP = 11;
        public static final int BUTTON_DPAD_DOWN = 12;
        public static final int BUTTON_DPAD_LEFT = 13;
        public static final int BUTTON_DPAD_RIGHT = 14;

        // SDL_GameControllerAxis
        public static final int AXIS_LEFTX = 0;
        public static final int AXIS_LEFTY = 1;
        public static final int AXIS_TRIGGERLEFT = 4;
        public static final int AXIS_TRIGGERRIGHT = 5;

        /** Length of the axis array the native side expects. */
        public static final int AXIS_COUNT = 6;
    }

    /** How a control is drawn and hit-tested. */
    public enum Kind {
        /** Round button, one circular hit target. */
        BUTTON,
        /** Rounded rectangle along a screen edge — L, Z and R. */
        SHOULDER,
        /** Analog stick: a base ring with a knob that follows the finger. */
        STICK,
        /** The settings handle: drawn as a hamburger rather than a glyph. */
        MENU
    }

    /** Which way a control's arrow points, for the C cluster. */
    public enum Dir { NONE, UP, RIGHT, DOWN, LEFT }

    public final Kind kind;
    public final String label;
    public final Dir dir;
    /** SDL button this control presses, or {@link Sdl#NONE} if it drives an axis. */
    public final int sdlButton;
    /** SDL axis this control drives to 1.0, or {@link Sdl#NONE} if it is a button. */
    public final int sdlAxis;

    TouchControl(Kind kind, String label, Dir dir, int sdlButton, int sdlAxis) {
        this.kind = kind;
        this.label = label;
        this.dir = dir;
        this.sdlButton = sdlButton;
        this.sdlAxis = sdlAxis;
    }

    /** True if this control is one of the four C-cluster members. */
    public boolean isCButton() {
        return this == C_UP || this == C_RIGHT || this == C_DOWN || this == C_LEFT;
    }

    /** The C cluster in diamond order: up, right, down, left. */
    public static final TouchControl[] C_CLUSTER = { C_UP, C_RIGHT, C_DOWN, C_LEFT };
}
