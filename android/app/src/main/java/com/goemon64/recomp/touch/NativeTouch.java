package com.goemon64.recomp.touch;

/**
 * The one seam between the on-screen controls and the game.
 *
 * <p>A few calls, none of them blocking: the overlay pushes
 * the whole virtual pad every time it changes, and native code reads it from the
 * game thread. See {@code include/goemon_touch.h} for what happens on the far side.
 *
 * <h2>Why every call is guarded</h2>
 * {@code libGoemon64.so} is loaded by {@code MainActivity}'s static initialiser, so in
 * the app these calls always resolve. The view does not assume that, though: the first
 * call latches whether the symbols are actually there, and every call after that is a
 * field test, so hosting the overlay in a process without the library degrades to a
 * pad that sends nothing rather than an UnsatisfiedLinkError. Nothing here throws.
 */
public final class NativeTouch {

    private NativeTouch() {}

    /**
     * Tri-state: unknown until the first call, then permanently available or not.
     * Never reset — a process either has the game's native library or it does not.
     */
    private static Boolean available;

    private static boolean available() {
        if (available == null) {
            try {
                // A no-op probe: clearing state that is already clear is harmless, and
                // it is the cheapest way to find out whether the symbols resolved.
                nativeClearState();
                available = Boolean.TRUE;
            } catch (UnsatisfiedLinkError e) {
                available = Boolean.FALSE;
            }
        }
        return available;
    }

    /**
     * Push the complete virtual pad state.
     *
     * @param buttonMask bit {@code n} set means SDL button {@code n} is held
     * @param axes       indexed by SDL axis; length may be short, the rest read neutral
     */
    public static void setState(int buttonMask, float[] axes) {
        if (!available()) {
            return;
        }
        try {
            nativeSetState(buttonMask, axes);
        } catch (UnsatisfiedLinkError ignored) {
            available = Boolean.FALSE;
        }
    }

    /**
     * Tell native code whether the overlay is currently driving input. Passing
     * {@code false} also drops the pad to neutral, so a button held at the instant
     * the overlay was hidden cannot stay held.
     */
    public static void setActive(boolean active) {
        if (!available()) {
            return;
        }
        try {
            nativeSetActive(active);
        } catch (UnsatisfiedLinkError ignored) {
            available = Boolean.FALSE;
        }
    }

    /**
     * Whether a native menu is capturing input, so the overlay can hide and let its
     * touches through to the menu underneath. Returns false if the native library is
     * not loaded.
     */
    public static boolean isMenuOpen() {
        if (!available()) {
            return false;
        }
        try {
            return nativeIsMenuOpen();
        } catch (UnsatisfiedLinkError ignored) {
            available = Boolean.FALSE;
            return false;
        }
    }

    /**
     * Ask the game to open (or close) its config menu, as the settings handle.
     *
     * <p>Not routed through the virtual pad: the menu toggle is event-driven on the
     * native side, so a bit in the polled button mask is never seen by it. See
     * {@code goemon64::touch::request_menu_toggle}.
     */
    public static void requestMenuToggle() {
        if (!available()) {
            return;
        }
        try {
            nativeRequestMenuToggle();
        } catch (UnsatisfiedLinkError ignored) {
            available = Boolean.FALSE;
        }
    }

    /**
     * Visibility mode chosen in the game's own settings menu, or {@code null} when the
     * native library is absent, in which case the locally stored layout value stands in.
     *
     * <p>The game config owns this rather than SharedPreferences so there is exactly
     * one source of truth: the player changes it in the menu they already have open,
     * and it is saved with the rest of their settings.
     */
    public static TouchLayout.Visibility mode() {
        if (!available()) {
            return null;
        }
        try {
            switch (nativeGetMode()) {
                case 1:  return TouchLayout.Visibility.ALWAYS;
                case 2:  return TouchLayout.Visibility.NEVER;
                default: return TouchLayout.Visibility.AUTO;
            }
        } catch (UnsatisfiedLinkError ignored) {
            available = Boolean.FALSE;
            return null;
        }
    }

    /**
     * Stick response from the game config, 0..100, or -1 when the native library is
     * absent, in which case the pad keeps its current setting.
     */
    public static int stickSensitivity() {
        if (!available()) {
            return -1;
        }
        try {
            return nativeGetStickSensitivity();
        } catch (UnsatisfiedLinkError ignored) {
            available = Boolean.FALSE;
            return -1;
        }
    }

    private static native void nativeSetState(int buttonMask, float[] axes);

    private static native int nativeGetStickSensitivity();

    private static native int nativeGetMode();

    private static native void nativeRequestMenuToggle();

    private static native boolean nativeIsMenuOpen();

    private static native void nativeSetActive(boolean active);

    private static native void nativeClearState();
}
