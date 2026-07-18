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

    bool take_android_autostart() {
        return g_autostart.exchange(false);
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
    LOGI("SDL_main -> game_main");
    return game_main(argc, argv);
}

} // extern "C"

#endif // __ANDROID__
