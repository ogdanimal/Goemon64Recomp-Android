#ifndef __GOEMON_SUPPORT_H__
#define __GOEMON_SUPPORT_H__

#include <functional>
#include <filesystem>
#include <string>
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

    // True when MainActivity was launched with the auto-start extra set, i.e.
    // this process is the "restart to title screen" half of a restart and should
    // boot the game rather than stopping at the launcher.
    bool android_autostart();

    // Called once the renderer has a working Vulkan device, with the name of the
    // device it selected. Records that name where the GPU driver settings can
    // show it. See android_glue.cpp; a no-op in a build without custom-driver
    // support. It deliberately does NOT clear the driver's boot latch -- see the
    // comment on the definition.
    void report_render_device(const char* device_name);

    // Android has no native file dialog, so picking a file means asking Java to
    // launch the system document picker: this returns immediately and the answer
    // arrives LATER, from android_pump_ui_callbacks() on the render thread. The
    // callback runs exactly once -- a cancelled or failed pick reports failure --
    // and the paths it receives are copies in the cache, not the picked documents
    // themselves, because native code needs something it can open by path.
    //
    // Only one pick can be outstanding; a second request fails immediately rather
    // than stranding the first caller's callback.
    void android_request_open_document(bool multiple,
        std::function<void(bool success, const std::list<std::filesystem::path>& paths)> callback);

    // Runs whatever Java has answered since the last call. Render thread only:
    // the callbacks are the ones registered above, and they touch the UI.
    void android_pump_ui_callbacks();

    // --- optional user-supplied Vulkan driver (see GpuDriverStore.java) ---

    // True only in a build with the loader compiled in. The settings category is
    // hidden otherwise, since selecting a driver would silently do nothing.
    bool android_gpu_driver_supported();

    struct GpuDriverEntry {
        std::string id;
        std::string name;
        std::string detail;   // version/vendor, or why this one may not work here
    };

    struct GpuDriverState {
        std::vector<GpuDriverEntry> drivers;
        std::string active_id;          // empty means the system driver
        std::string device;             // GPU the renderer came up on, empty if unrecorded
        std::string loader;             // what the loader did last launch, in words
        // A driver is in use that nobody has confirmed renders the game yet, so
        // the crash latch is armed and only the user can clear it.
        bool needs_confirmation = false;
    };

    // All of these are small file operations behind a JNI call, safe to make from
    // the render thread, and Java remains the only writer of the driver store.
    GpuDriverState android_gpu_driver_state();
    void android_gpu_driver_select(const std::string& id);   // empty id = system driver
    void android_gpu_driver_remove(const std::string& id);
    // Keep the running driver: clears the crash latch and marks it confirmed. The
    // id is passed so a selection changed in the meantime cannot be confirmed by
    // an answer that was about a different driver.
    void android_gpu_driver_confirm(const std::string& id);
    // A confirmed driver has kept the renderer alive; clears the latch for this
    // launch. Does nothing for an unconfirmed one.
    void android_gpu_driver_note_survived();
    // Import a driver the user picks. Asynchronous exactly like the file dialog
    // above, and answered from the same pump: on success the message is the
    // driver's name, on failure it is why it was rejected (empty if cancelled).
    void android_gpu_driver_request_import(
        std::function<void(bool success, const std::string& message)> callback);

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
