#ifndef __GOEMON_TOUCH_H__
#define __GOEMON_TOUCH_H__

#include <cstdint>

// On-screen ("tactile") controls for touchscreen devices.
//
// The overlay itself is drawn and hit-tested in Java (TouchOverlayView), because
// nothing about it needs the renderer: it is a plain Android View composited over
// SDL's SurfaceView, so layout changes never require an NDK rebuild
// (see docs/touch-controls.md).
//
// What crosses into native code is only the RESULT: a virtual gamepad shaped
// exactly like an SDL game controller. input.cpp merges it into
// controller_button_state() / controller_axis_state(), the same two functions
// every physical pad flows through, which is what makes the overlay a genuine
// extra input source rather than a keyboard alias:
//
//   * every binding in Settings -> Controls applies to it unchanged,
//   * a rebind can never detach it (it has no bindings of its own),
//   * it merges with a physical pad instead of fighting it -- a stick held on
//     the touch overlay and one held on a real controller OR together, exactly
//     as two physical pads already do, and
//   * the analog camera, C-button masking and mods need no knowledge of it.
//
// Buttons and axes are indexed by the SDL_GameControllerButton /
// SDL_GameControllerAxis enum values. They are passed as plain ints so this
// header stays free of SDL, and are range-checked on the way in: the values
// originate in Java and a mismatched build must not index off the end.
namespace goemon64 {
    namespace touch {
        // Highest SDL_GameControllerButton this bridge carries. SDL 2.30's
        // SDL_CONTROLLER_BUTTON_MAX is 21; the mask is 32 bits, so there is room
        // to spare. Static-asserted against SDL's real value in android_touch.cpp
        // so a future SDL bump fails the build here rather than silently
        // truncating the mask.
        inline constexpr int button_count = 32;
        inline constexpr int axis_count = 6;

        // Replace the whole virtual pad state in one call. Written from the
        // Android UI thread, read from the game thread; see android_touch.cpp for
        // why a torn read between the mask and the axes is harmless.
        void set_state(uint32_t button_mask, const float* axes, int axis_len);

        // Drop everything to neutral. Used when the overlay is hidden, the app
        // loses focus, or the activity goes away -- otherwise a button held at
        // the moment the overlay disappeared would stay held forever.
        void clear_state();

        // Ask the UI to open/close the config menu, as the on-screen settings handle.
        // Queues a real controller-button event rather than setting a bit, because the
        // menu toggle is event-driven while gameplay input is polled; see the
        // implementation for why that distinction matters.
        void request_menu_toggle();

        bool button_held(int sdl_button);
        float axis_value(int sdl_axis);
    }
}

#endif
