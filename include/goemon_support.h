#ifndef __GOEMON_SUPPORT_H__
#define __GOEMON_SUPPORT_H__

#include <functional>
#include <filesystem>
#include <vector>
#include <optional>
#include <list>

namespace goemon64 {
    std::filesystem::path get_program_path();
    std::filesystem::path get_asset_path(const char* asset);
    void open_file_dialog(std::function<void(bool success, const std::filesystem::path& path)> callback);
    void open_file_dialog_multiple(std::function<void(bool success, const std::list<std::filesystem::path>& paths)> callback);
    void show_error_message_box(const char *title, const char *message);

// Android: app-private data directory, set by MainActivity.nativeInit().
// Implemented in android_glue.cpp; used by get_program_path().
#ifdef __ANDROID__
    const std::filesystem::path& android_program_path();
    std::filesystem::path android_rom_path();

    // TEMPORARY Bug-6 crash diagnostics (android_diag.cpp). Remove with the fix.
    namespace diag {
        enum Phase : int { Foreground = 0, Background = 1, Resuming = 2 };
        void install_crash_handler();
        void set_rdram_base(const void* base);
        void set_phase(int phase);
    }
#endif

// Apple specific methods that usually require Objective-C. Implemented in support_apple.mm.
#ifdef __APPLE__
    void dispatch_on_ui_thread(std::function<void()> func);
    std::optional<std::filesystem::path> get_application_support_directory();
    std::filesystem::path get_bundle_resource_directory();
    std::filesystem::path get_bundle_directory();
#endif
}

#endif
