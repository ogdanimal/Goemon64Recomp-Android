#include "ultramodern/ultramodern.hpp"

#include "recomp_ui.h"
#include "goemon_support.h"
#include "ui_utils.h"

// On platforms whose file dialog is asynchronous -- Android, where picking a file
// means the system document picker and a trip through another activity -- the
// callback the caller registered has to be run somewhere. Here, once a frame, on
// the render thread, which is where the blocking desktop implementations run
// theirs. A no-op everywhere else.
void recompui::pump_file_dialogs() {
#if defined(__ANDROID__)
    goemon64::android_pump_ui_callbacks();
#endif
}

recompui::Color recompui::lerp_color(const recompui::Color& a, const recompui::Color& b, float factor) {
    return recompui::Color{
        static_cast<uint8_t>(std::lerp(float(a.r), float(b.r), factor)),
        static_cast<uint8_t>(std::lerp(float(a.g), float(b.g), factor)),
        static_cast<uint8_t>(std::lerp(float(a.b), float(b.b), factor)),
        static_cast<uint8_t>(std::lerp(float(a.a), float(b.a), factor)),
    };
}

recompui::Color recompui::get_pulse_color(uint32_t pulse_milliseconds) {
    uint64_t millis = std::chrono::duration_cast<std::chrono::milliseconds>(ultramodern::time_since_start()).count();
    uint32_t anim_offset = millis % pulse_milliseconds;
    
    float factor = std::abs((2.0f * anim_offset / pulse_milliseconds) - 1.0f);
    return lerp_color(Color{ 23, 214, 232, 255 }, Color{ 162, 239, 246, 255 }, factor);
}
