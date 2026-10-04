#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#include <jni.h>

#include "SDL.h"

#include "goemon_config.h"
#include "recomp_input.h"
#include "recomp_ui.h"

#include "goemon_touch.h"

// Virtual-gamepad state fed by the Java on-screen controls. See goemon_touch.h
// for why this is the whole of the native side of the feature.
//
// THREADING. set_state runs on the Android UI thread (the View's touch handler);
// button_held / axis_value run on the game thread out of
// controller_button_state / controller_axis_state. Everything is a relaxed
// atomic, with no lock and no allocation, because the touch handler must never
// block on the game thread -- a stall there is a dropped frame of input latency
// on the one input path that has no physical detent to mask it.
//
// A read CAN tear across the mask/axis boundary: the game thread may see a new
// button mask with the previous frame's stick, for at most one frame. That is
// deliberate and harmless -- it is a sub-16 ms skew on two independent controls,
// which is smaller than the skew a real USB pad already has between its own
// reports. Taking a lock to remove it would trade an invisible artifact for a
// visible one.
namespace {
    // If SDL grows past what the mask can carry, catch it here rather than
    // silently dropping the high buttons.
    static_assert(SDL_CONTROLLER_BUTTON_MAX <= goemon64::touch::button_count,
        "SDL_GameControllerButton no longer fits the touch button mask");
    static_assert(SDL_CONTROLLER_AXIS_MAX <= goemon64::touch::axis_count,
        "SDL_GameControllerAxis no longer fits the touch axis array");

    // The Java overlay names its outputs with its own copy of these enum values
    // (TouchControl.java), because JNI has no way to read a C enum. That copy is
    // only correct as long as SDL's numbering does not move, and if it ever did,
    // the failure would be silent and awful -- every on-screen button would press
    // the wrong thing, on a control scheme whose whole point is that it has no
    // labels to contradict it. Pin the exact values here so an SDL bump breaks the
    // build instead, with a message naming the file to fix.
#define GOEMON_PIN_SDL(sym, expected)     static_assert((int)(sym) == (expected),         #sym " changed value; update SDL_* constants in "         "android/app/src/main/java/com/goemon64/recomp/touch/TouchControl.java")

    GOEMON_PIN_SDL(SDL_CONTROLLER_BUTTON_A, 0);
    GOEMON_PIN_SDL(SDL_CONTROLLER_BUTTON_B, 1);
    GOEMON_PIN_SDL(SDL_CONTROLLER_BUTTON_BACK, 4);
    GOEMON_PIN_SDL(SDL_CONTROLLER_BUTTON_START, 6);
    GOEMON_PIN_SDL(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 9);
    GOEMON_PIN_SDL(SDL_CONTROLLER_BUTTON_DPAD_UP, 11);
    GOEMON_PIN_SDL(SDL_CONTROLLER_BUTTON_DPAD_DOWN, 12);
    GOEMON_PIN_SDL(SDL_CONTROLLER_BUTTON_DPAD_LEFT, 13);
    GOEMON_PIN_SDL(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 14);
    GOEMON_PIN_SDL(SDL_CONTROLLER_AXIS_LEFTX, 0);
    GOEMON_PIN_SDL(SDL_CONTROLLER_AXIS_LEFTY, 1);
    GOEMON_PIN_SDL(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 4);
    GOEMON_PIN_SDL(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 5);

#undef GOEMON_PIN_SDL

    std::atomic<uint32_t> touch_buttons{ 0 };
    std::array<std::atomic<float>, goemon64::touch::axis_count> touch_axes{};
}

namespace goemon64 {
    namespace touch {
        void set_state(uint32_t button_mask, const float* axes, int axis_len) {
            touch_buttons.store(button_mask, std::memory_order_relaxed);

            const int count = std::min(axis_len, static_cast<int>(touch_axes.size()));
            for (int i = 0; i < count; i++) {
                // Clamped here rather than trusted: these come across JNI, and a
                // NaN or an out-of-range magnitude would propagate straight into
                // the N64 stick value.
                float value = axes[i];
                if (!(value == value)) { // NaN
                    value = 0.0f;
                }
                touch_axes[i].store(std::clamp(value, -1.0f, 1.0f), std::memory_order_relaxed);
            }
            // A short array means "the rest are neutral", not "leave them alone" --
            // otherwise a shrinking payload would strand a stuck axis.
            for (int i = count; i < static_cast<int>(touch_axes.size()); i++) {
                touch_axes[i].store(0.0f, std::memory_order_relaxed);
            }
        }

        void clear_state() {
            touch_buttons.store(0, std::memory_order_relaxed);
            for (auto& axis : touch_axes) {
                axis.store(0.0f, std::memory_order_relaxed);
            }
        }

        bool button_held(int sdl_button) {
            if (sdl_button < 0 || sdl_button >= button_count) {
                return false;
            }
            return (touch_buttons.load(std::memory_order_relaxed) & (1u << sdl_button)) != 0;
        }

        void request_menu_toggle() {
            // The on-screen handle cannot open the menu through the virtual pad, and
            // this is why: gameplay inputs are POLLED (controller_button_state), but
            // the menu toggle is EVENT-driven -- ui_state.cpp opens the config context
            // when it dequeues an SDL_CONTROLLERBUTTONDOWN whose button matches the
            // TOGGLE_MENU binding. A bit set in the polled mask is never seen by that
            // path, so the handle set the bit and nothing happened.
            //
            // So queue a real event instead of faking the state. Going through the
            // same queue the physical pad uses means the handle inherits the rest of
            // the behaviour for free -- including opening AND closing, and following
            // a rebind, because the consumer compares against whatever TOGGLE_MENU is
            // bound to at the time.
            int button = SDL_CONTROLLER_BUTTON_BACK;
            const recomp::InputField& primary =
                recomp::get_input_binding(recomp::GameInput::TOGGLE_MENU, 0, recomp::InputDevice::Controller);
            const recomp::InputField& secondary =
                recomp::get_input_binding(recomp::GameInput::TOGGLE_MENU, 1, recomp::InputDevice::Controller);
            // input_type 0 is InputType::None, i.e. unbound -- the same check the
            // consumer makes. Falling back to BACK keeps the handle working even if
            // the player has cleared both bindings.
            if (primary.input_type != 0) {
                button = primary.input_id;
            }
            else if (secondary.input_type != 0) {
                button = secondary.input_id;
            }

            SDL_Event down{};
            down.type = SDL_CONTROLLERBUTTONDOWN;
            down.cbutton.type = SDL_CONTROLLERBUTTONDOWN;
            down.cbutton.timestamp = SDL_GetTicks();
            down.cbutton.which = 0;
            down.cbutton.button = static_cast<Uint8>(button);
            down.cbutton.state = SDL_PRESSED;
            recompui::queue_event(down);

            // Paired release: with the menu already open the same button maps to
            // Escape, and RmlUi wants a key-up for the key-down it was given.
            SDL_Event up = down;
            up.type = SDL_CONTROLLERBUTTONUP;
            up.cbutton.type = SDL_CONTROLLERBUTTONUP;
            up.cbutton.state = SDL_RELEASED;
            recompui::queue_event(up);
        }

        float axis_value(int sdl_axis) {
            if (sdl_axis < 0 || sdl_axis >= axis_count) {
                return 0.0f;
            }
            return touch_axes[sdl_axis].load(std::memory_order_relaxed);
        }
    }
}

extern "C" {

JNIEXPORT void JNICALL
Java_com_goemon64_recomp_touch_NativeTouch_nativeSetState(JNIEnv* env, jclass /*clazz*/,
        jint buttonMask, jfloatArray axes) {
    float values[goemon64::touch::axis_count] = { 0.0f };
    jsize len = 0;

    if (axes != nullptr) {
        len = std::min(env->GetArrayLength(axes),
                       static_cast<jsize>(goemon64::touch::axis_count));
        if (len > 0) {
            // GetFloatArrayRegion copies into our own stack buffer, so there is no
            // pinned array left live across the store below and nothing to release
            // on an early return.
            env->GetFloatArrayRegion(axes, 0, len, values);
        }
    }

    // jint is signed; the mask is a bit set, so reinterpret rather than convert.
    goemon64::touch::set_state(static_cast<uint32_t>(buttonMask), values, static_cast<int>(len));
}

JNIEXPORT void JNICALL
Java_com_goemon64_recomp_touch_NativeTouch_nativeSetActive(JNIEnv* /*env*/, jclass /*clazz*/,
        jboolean active) {
    // Hiding the overlay drops the pad to neutral, so a button held at the instant
    // it disappeared cannot stay held.
    if (active != JNI_TRUE) {
        goemon64::touch::clear_state();
    }
}

JNIEXPORT void JNICALL
Java_com_goemon64_recomp_touch_NativeTouch_nativeClearState(JNIEnv* /*env*/, jclass /*clazz*/) {
    goemon64::touch::clear_state();
}

JNIEXPORT void JNICALL
Java_com_goemon64_recomp_touch_NativeTouch_nativeRequestMenuToggle(JNIEnv* /*env*/, jclass /*clazz*/) {
    goemon64::touch::request_menu_toggle();
}

// Visibility mode from the game's own config, so the menu option is the single
// source of truth and the Java side has nothing of its own to keep in step. Ints
// rather than an enum because JNI cannot read one; kept in sync with
// TouchLayout.Visibility by nativeGetMode's contract in NativeTouch.
JNIEXPORT jint JNICALL
Java_com_goemon64_recomp_touch_NativeTouch_nativeGetMode(JNIEnv* /*env*/, jclass /*clazz*/) {
    switch (goemon64::get_touch_controls_mode()) {
        case goemon64::TouchControlsMode::On:  return 1;
        case goemon64::TouchControlsMode::Off: return 2;
        default:                               return 0; // Auto
    }
}

// Stick response, 0..100. Read on the same poll as the visibility mode.
JNIEXPORT jint JNICALL
Java_com_goemon64_recomp_touch_NativeTouch_nativeGetStickSensitivity(JNIEnv* /*env*/, jclass /*clazz*/) {
    return goemon64::get_touch_stick_sensitivity();
}

// Whether a menu that blocks game input is on screen -- the in-app launcher, the
// config menu, a modal prompt.
//
// The overlay polls this instead of being told. A push would mean an upcall from
// whichever thread happened to change the UI state, with a cached JavaVM, a
// per-thread AttachCurrentThread, and a global class reference to keep alive across
// the activity being recreated. A poll a few times a second cannot leak, deadlock, or
// fire into a dead activity -- and the deadline it has to meet is a human noticing
// the pad is still drawn, which is nowhere near tight enough to justify the
// alternative.
//
// It reads the per-frame snapshot, NOT recompui::is_context_capturing_input(). That
// one takes ui_state_mutex, which the render thread holds for the whole UI pass of
// every frame -- and this runs on Android's main thread. Waiting there would stall
// touch handling behind the UI, and worse, would freeze the app outright if the
// render thread raised an error dialog while holding the mutex:
// SDL_ShowSimpleMessageBox waits for the main thread to show and dismiss the dialog,
// and the main thread would be waiting for the mutex.
JNIEXPORT jboolean JNICALL
Java_com_goemon64_recomp_touch_NativeTouch_nativeIsMenuOpen(JNIEnv* /*env*/, jclass /*clazz*/) {
    return recompui::is_context_capturing_input_snapshot() ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
