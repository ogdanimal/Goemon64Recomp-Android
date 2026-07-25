// Android entry glue for the Goemon 64 recompilation.
//
// SDLActivity (Java) loads libGoemon64.so and, on its own thread, dlsym()s and
// calls "SDL_main". We keep the portable game entry point in main.cpp as
// game_main(argc, argv) on Android, and bridge SDL_main -> game_main here.
//
// Before the game starts, MainActivity calls nativeInit(dataDir) via JNI. That:
//   1. Points the recomp config/asset lookup at app-private storage by setting
//      APP_FOLDER_PATH (config.cpp's Linux/Android branch honors it) and by
//      recording the dir for get_program_path().
//   2. Registers the SAF-copied ROM with librecomp so is_rom_valid() is true and
//      the RmlUi launcher jumps straight to "Start Game".
//
// Assets (fonts, .rml, .rcss, recompcontrollerdb.txt) are shipped in the APK and
// extracted to <dataDir> by the Java side before nativeInit runs.

#if defined(__ANDROID__)

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <jni.h>
#include <android/log.h>

#include "SDL2/SDL.h"

#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "goemon_support.h"

#if defined(GOEMON_CUSTOM_VULKAN_DRIVER)
#include <dlfcn.h>
#include <vulkan/vulkan_core.h>
#include <adrenotools/driver.h>

// Declared rather than included: pulling plume_vulkan.h in here would drag volk
// and the whole render interface into this translation unit for one function.
// PFN_vkGetInstanceProcAddr is a standard Vulkan typedef, identical on both
// sides, and any drift shows up as an undefined symbol at link time, not at run
// time. Only declared under the same define that makes plume define it.
namespace plume {
    void SetCustomVulkanLoader(PFN_vkGetInstanceProcAddr getInstanceProcAddr);
}
#endif

#define LOG_TAG "Goemon64"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// The supported games table lives in main.cpp; we only need entry 0's id.
extern std::vector<recomp::GameEntry> supported_games;

// Portable game entry, defined in main.cpp (renamed from main() on Android).
extern int game_main(int argc, char** argv);

namespace {
    // App-private data directory handed to us by MainActivity.nativeInit().
    std::filesystem::path g_data_dir;

    // Where MainActivity.onDestroy() should relaunch us, if anywhere. Written on
    // the UI thread (from a menu button) and read on the UI thread in onDestroy,
    // but the shutdown in between crosses threads, so keep it atomic.
    std::atomic<int> g_restart_target{ static_cast<int>(goemon64::RestartTarget::None) };

    // Set from the Intent extra when this process is a restart-to-title relaunch.
    // Written on the UI thread in nativeInit, consumed on the graphics thread.
    std::atomic_bool g_autostart{ false };
}

namespace goemon64 {
    // get_program_path() (support.cpp) returns this on Android so that assets and
    // recompcontrollerdb.txt resolve under app-private storage.
    const std::filesystem::path& android_program_path() {
        return g_data_dir;
    }

    // The SAF-copied ROM path (<dataDir>/mnsg.z64). game_main() registers it with
    // librecomp AFTER register_game() has run; nativeInit runs too early for that
    // (the game_roms map is still empty -> select_rom returns OtherError).
    std::filesystem::path android_rom_path() {
        return g_data_dir / "mnsg.z64";
    }

    bool android_autostart() {
        return g_autostart.load();
    }

    void request_restart(RestartTarget target) {
        g_restart_target.store(static_cast<int>(target));
        LOGI("request_restart(%d) -> quitting for relaunch", static_cast<int>(target));
        // Reuse the normal shutdown: it flushes saves, joins the saving thread
        // and tears the renderer down before game_main() returns, at which point
        // SDLActivity finishes the activity and onDestroy() does the relaunch.
        ultramodern::quit();
    }
}

extern "C" {

JNIEXPORT void JNICALL
Java_com_goemon64_recomp_MainActivity_nativeInit(JNIEnv* env, jobject /*thiz*/, jstring dataPath, jboolean autostart) {
    const char* c_data = env->GetStringUTFChars(dataPath, nullptr);
    g_data_dir = std::filesystem::path{c_data};
    g_autostart.store(autostart == JNI_TRUE);
    LOGI("nativeInit: data dir = %s, autostart = %d", c_data, static_cast<int>(autostart == JNI_TRUE));

    // config.cpp's get_app_folder_path() checks APP_FOLDER_PATH first on Linux/Android.
    setenv("APP_FOLDER_PATH", c_data, /*overwrite=*/1);
    env->ReleaseStringUTFChars(dataPath, c_data);

    // NOTE: ROM registration is deferred to game_main() (see main.cpp), which runs
    // it after recomp::register_game(). Calling select_rom() here fails with
    // OtherError because the game hasn't been registered yet at nativeInit time.
}

// Loads a user-supplied Vulkan driver (Mesa Turnip and friends) through
// libadrenotools and hands its vkGetInstanceProcAddr to plume, in place of the
// system Vulkan loader volk would otherwise dlopen. Called by MainActivity
// before super.onCreate() starts the SDL thread, which is the only window in
// which this can be installed -- the renderer initialises Vulkan from there.
//
// The whole body is compiled out unless -PcustomDriver=true built this APK; the
// JNI entry point itself always exists so the Java call never hits an
// UnsatisfiedLinkError. With no driver file present it is a no-op, so even a
// custom-driver build behaves normally until someone puts a driver in place.
JNIEXPORT void JNICALL
Java_com_goemon64_recomp_MainActivity_nativeInitCustomDriver(JNIEnv* env, jobject /*thiz*/,
                                                             jstring hookLibDir, jstring driverDir) {
#if defined(GOEMON_CUSTOM_VULKAN_DRIVER)
    const char* c_hook = env->GetStringUTFChars(hookLibDir, nullptr);
    const char* c_driver = env->GetStringUTFChars(driverDir, nullptr);
    const std::string hook_dir = c_hook;
    // MUST end in a separator: adrenotools_open_libvulkan builds the driver path
    // as customDriverDir + customDriverName with nothing between them, so a
    // directory without the trailing slash makes its stat() miss and the whole
    // call return null with no diagnostic (driver.cpp:45 in the vendored copy).
    std::string driver_dir = c_driver;
    if (!driver_dir.empty() && driver_dir.back() != '/') {
        driver_dir.push_back('/');
    }
    env->ReleaseStringUTFChars(hookLibDir, c_hook);
    env->ReleaseStringUTFChars(driverDir, c_driver);

    // The driver is identified by whatever single .so sits in the directory --
    // an .adpkg names it in meta.json, but reading that here would mean shipping
    // a JSON parser into the glue for one string. More than one file means we
    // cannot tell which was intended, so refuse rather than pick.
    std::error_code ec;
    std::string driver_name;
    int so_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(driver_dir, ec)) {
        if (entry.path().extension() == ".so") {
            driver_name = entry.path().filename().string();
            ++so_count;
        }
    }
    if (so_count == 0) {
        LOGI("custom driver: nothing in %s, using the system Vulkan driver", driver_dir.c_str());
        return;
    }
    if (so_count > 1) {
        LOGE("custom driver: %d .so files in %s, refusing to guess which one to load", so_count, driver_dir.c_str());
        return;
    }

    // tmpLibDir is null because it is only consulted below API 29, where
    // libadrenotools falls back to memfd; minSdk here is 28, so a device at
    // exactly 28 can legitimately fail this call and drop to the system driver.
    // hookLibDir MUST be nativeLibraryDir and the APK must use legacy jniLibs
    // packaging, or the hook silently does nothing (see adrenotools/driver.h).
    void* libvulkan = adrenotools_open_libvulkan(
        RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
        /*tmpLibDir=*/nullptr,
        hook_dir.c_str(),
        driver_dir.c_str(),
        driver_name.c_str(),
        /*fileRedirectDir=*/nullptr,
        /*userMappingHandle=*/nullptr);
    if (libvulkan == nullptr) {
        // It reports no reason, so log everything a reader would otherwise have to
        // guess at. Every one of its early exits is a silent `return nullptr`.
        LOGE("custom driver: adrenotools_open_libvulkan failed -- driver '%s' dir '%s' hooks '%s'",
             driver_name.c_str(), driver_dir.c_str(), hook_dir.c_str());
        return;
    }

    auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(libvulkan, "vkGetInstanceProcAddr"));
    if (get_instance_proc_addr == nullptr) {
        LOGE("custom driver: vkGetInstanceProcAddr missing from %s (%s)", driver_name.c_str(), dlerror());
        return;
    }

    plume::SetCustomVulkanLoader(get_instance_proc_addr);
    // Deliberately hedged: a non-null handle does NOT prove the custom driver is
    // in use. adrenotools can succeed here and still fall back to the system
    // driver, or enumerate zero devices, if the hook libraries were not found.
    // The '[plume] Using device' line later in startup is the authority on which
    // driver actually got loaded -- read that, not this.
    LOGI("custom driver: requested %s via libadrenotools (hooks: %s) -- confirm with the [plume] device line",
         driver_name.c_str(), hook_dir.c_str());
#else
    (void)env;
    (void)hookLibDir;
    (void)driverDir;
#endif
}

JNIEXPORT void JNICALL
Java_com_goemon64_recomp_MainActivity_nativeDestroy(JNIEnv* /*env*/, jobject /*thiz*/) {
    LOGI("nativeDestroy");
}

// Read by MainActivity.onDestroy() to decide whether to relaunch. See
// goemon64::RestartTarget for the values.
JNIEXPORT jint JNICALL
Java_com_goemon64_recomp_MainActivity_nativeGetRestartTarget(JNIEnv* /*env*/, jobject /*thiz*/) {
    return static_cast<jint>(g_restart_target.load());
}

// SDL's Android bootstrap calls this after loadLibraries()/nativeInit().
int SDL_main(int argc, char** argv) {
    // Safety net for the storage guard (M4). If we reach here with g_data_dir
    // unset, nativeInit never ran — the process was started for a MainActivity
    // that bailed on a missing storage volume, and SDLActivity nonetheless started
    // SDL_main on the finishing activity (the RESUMED-transition race that the
    // Java-side finish()-in-onCreate usually prevents but cannot guarantee across
    // OEMs). Booting game_main against an empty data dir would resolve assets/ROM
    // from "" and crash; abort cleanly instead. The Java guard has already bounced
    // the user to LauncherActivity's "card missing" dialog.
    if (g_data_dir.empty()) {
        LOGE("SDL_main: data dir unset (nativeInit was skipped); aborting boot");
        return 0;
    }
    LOGI("SDL_main -> game_main");
    return game_main(argc, argv);
}

} // extern "C"

#endif // __ANDROID__
