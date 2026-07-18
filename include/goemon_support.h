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

    // Restarting the game means restarting the process: librecomp latches
    // `exited`, `game_status` and rdram one-way, so there is no in-process way
    // back to a cold boot. request_restart() records where MainActivity should
    // land after the normal shutdown path finishes, then calls
    // ultramodern::quit() so saves are flushed and the renderer torn down
    // cleanly. MainActivity.onDestroy() reads the target back and relaunches.
    //
    // The relaunch cannot race the save flush: onDestroy only runs after
    // game_main() has returned, i.e. after recomp::start() joined the saving
    // thread, so nothing is still in flight by the time the process is killed.
    enum class RestartTarget : int {
        None = 0,        // normal quit; the process exits
        // "App menu" is this port's own launcher screen (Start Game / Controls /
        // Settings / Mods / Exit), NOT the game's title-screen menu -- that is
        // what TitleScreen gets you.
        AppMenu = 1,     // relaunch and stop at the in-app launcher
        TitleScreen = 2, // relaunch and auto-start the game, skipping the launcher
    };
    void request_restart(RestartTarget target);

    // True (once) when MainActivity was launched with the auto-start extra set,
    // i.e. this process is the "restart to title screen" half of a restart.
    //
    // Consuming rather than peeking is deliberate: the caller is the per-frame
    // UI hook, and start_game() must happen exactly once and no earlier. It
    // cannot be hoisted to startup either -- the VI thread branches on
    // is_game_started(), and if that is true on the first tick it skips
    // set_dummy_vi() and then dereferences the still-null VI state.
    bool take_android_autostart();

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
