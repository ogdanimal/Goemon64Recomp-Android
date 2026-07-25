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
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <jni.h>
#include <android/log.h>

#include "SDL2/SDL.h"

#include "json/json.hpp"

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

namespace {
    // Outcome of the load attempt, written to state/last_status for the driver UI
    // to show. MUST stay in sync with GpuDriverStore's STATUS_ constants.
    enum class CustomDriverStatus : int {
        NotAttempted = 0,
        // The loader accepted the driver and plume will be handed it. NOT proof the
        // driver is in use -- adrenotools can succeed and still fall back to the
        // system driver. The device name written by the render context is the
        // authority, which is why the UI reports both.
        Requested = 1,
        OpenFailed = 2,
        NoProcAddr = 3,
        BadArguments = 4,
    };

    // Where the driver UI reads our results from. Captured at init so the
    // render-context callback below can report without being handed a path.
    std::string g_driver_state_dir;

    void write_driver_state(const std::string& name, const std::string& value) {
        if (g_driver_state_dir.empty()) {
            return;
        }
        std::ofstream out(g_driver_state_dir + "/" + name, std::ios::binary | std::ios::trunc);
        if (out.is_open()) {
            out << value;
        }
    }

    void set_driver_status(CustomDriverStatus status) {
        write_driver_state("last_status", std::to_string(static_cast<int>(status)));
    }
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

    // ------------------------------------------------------------ calling Java
    //
    // Everything the game needs from the Android framework -- the document
    // picker, the GPU driver store -- lives on MainActivity, so the game side
    // calls back into it. Two rules make that safe.
    //
    // The activity and its method IDs are resolved ONCE, in nativeInit, and kept
    // as global references. They cannot be looked up later: nativeInit runs on
    // the UI thread with app classes on the call stack, whereas the render thread
    // was created by the renderer and never entered from Java, so FindClass there
    // searches only the system class loader and would not find our classes at all.
    //
    // The global reference to the activity is deliberately never released. It is
    // the activity that hosts the game for the whole life of the process, and the
    // process is taken down wholesale when it finishes (MainActivity.onDestroy),
    // so there is no window in which releasing it would matter.
    jobject g_activity = nullptr;
    jclass g_activity_class = nullptr;
    jmethodID g_mid_request_open_document = nullptr;
    jmethodID g_mid_request_driver_import = nullptr;
    jmethodID g_mid_gpu_state = nullptr;
    jmethodID g_mid_gpu_select = nullptr;
    jmethodID g_mid_gpu_remove = nullptr;
    jmethodID g_mid_gpu_confirm = nullptr;
    jmethodID g_mid_gpu_note_survived = nullptr;

    // Guards both pending-answer slots below. Java answers on one of its own
    // threads; the game reads on the render thread.
    std::mutex g_java_answer_mutex;

    std::function<void(bool, const std::list<std::filesystem::path>&)> g_dialog_callback;
    bool g_dialog_answered = false;
    bool g_dialog_success = false;
    std::list<std::filesystem::path> g_dialog_paths;

    std::function<void(bool, const std::string&)> g_import_callback;
    bool g_import_answered = false;
    bool g_import_success = false;
    std::string g_import_message;

    std::string to_string(JNIEnv* env, jstring value) {
        if (env == nullptr || value == nullptr) {
            return {};
        }
        const char* chars = env->GetStringUTFChars(value, nullptr);
        std::string out = (chars != nullptr) ? chars : "";
        env->ReleaseStringUTFChars(value, chars);
        return out;
    }

    // A JNIEnv for the calling thread, attaching it if this is the first call
    // from it. SDL owns the attachment and detaches the thread when it exits, so
    // this must not be paired with a manual DetachCurrentThread.
    JNIEnv* java_env() {
        if (g_activity == nullptr) {
            return nullptr;
        }
        return static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    }

    // An exception left pending would abort the process at the next JNI call, in
    // a place with nothing to do with the cause, so clear it here and say what it
    // was. Every method called through this bridge is expected not to throw.
    void clear_java_exception(JNIEnv* env, const char* what) {
        if (env != nullptr && env->ExceptionCheck()) {
            LOGE("java call failed: %s", what);
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }

    void call_java_void(jmethodID method, const char* what) {
        JNIEnv* env = java_env();
        if (env == nullptr || method == nullptr) {
            return;
        }
        env->CallVoidMethod(g_activity, method);
        clear_java_exception(env, what);
    }

    void call_java_void_string(jmethodID method, const std::string& argument, const char* what) {
        JNIEnv* env = java_env();
        if (env == nullptr || method == nullptr) {
            return;
        }
        jstring value = env->NewStringUTF(argument.c_str());
        env->CallVoidMethod(g_activity, method, value);
        clear_java_exception(env, what);
        env->DeleteLocalRef(value);
    }

    std::string call_java_string(jmethodID method, const char* what) {
        JNIEnv* env = java_env();
        if (env == nullptr || method == nullptr) {
            return {};
        }
        auto result = static_cast<jstring>(env->CallObjectMethod(g_activity, method));
        clear_java_exception(env, what);
        if (result == nullptr) {
            return {};
        }
        std::string out = to_string(env, result);
        env->DeleteLocalRef(result);
        return out;
    }
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

    // Called by the render context once a Vulkan device is up, with the name of
    // the device that was actually selected.
    //
    // This is the ONLY authority on whether an optional user-supplied Vulkan
    // driver is in use: loading one succeeds even when the system driver ends up
    // being used, so the loader's own result cannot answer the question. The
    // driver settings show this name so a bug report says which driver rendered.
    //
    // It deliberately does NOT clear the boot latch, even though this is the
    // obvious place for it. Reaching here means Vulkan initialised, which is
    // earlier than the failure that matters most: the Adreno fault this whole
    // feature exists for kills the process about a second AFTER rendering starts.
    // Clearing the latch here would therefore disarm it just before the crash it
    // is meant to catch, leaving the crash loop unbroken. Nor does initialising
    // prove anything is on screen. What clears the latch is the user answering
    // the confirmation prompt, or -- for a driver already confirmed -- the
    // renderer having kept going for a while; see recompui::tick_gpu_driver.
    //
    // Outside the extern "C" block below: goemon_support.h declares it as ordinary
    // C++ and it must keep that linkage.
    void report_render_device(const char* device_name) {
#if defined(GOEMON_CUSTOM_VULKAN_DRIVER)
        if (g_driver_state_dir.empty()) {
            return;
        }
        write_driver_state("last_device", (device_name != nullptr) ? device_name : "");
        LOGI("custom driver: renderer came up on '%s'", (device_name != nullptr) ? device_name : "?");
#else
        (void)device_name;
#endif
    }

    // ------------------------------------------------------ picking a document

    void android_request_open_document(bool multiple,
            std::function<void(bool, const std::list<std::filesystem::path>&)> callback) {
        bool refuse = false;
        {
            std::lock_guard lock{ g_java_answer_mutex };
            // One picker at a time. A second request would overwrite the first
            // callback, and the caller waiting on it would never hear anything.
            refuse = (g_dialog_callback != nullptr) || (g_activity == nullptr);
            if (!refuse) {
                g_dialog_callback = std::move(callback);
                g_dialog_answered = false;
            }
        }
        if (refuse) {
            // Answer outside the lock: the callback may open a menu, which can
            // come back through this file.
            callback(false, {});
            return;
        }

        JNIEnv* env = java_env();
        if (env != nullptr && g_mid_request_open_document != nullptr) {
            env->CallVoidMethod(g_activity, g_mid_request_open_document, multiple ? JNI_TRUE : JNI_FALSE);
            clear_java_exception(env, "requestOpenDocument");
        }
    }

    void android_pump_ui_callbacks() {
        std::function<void(bool, const std::list<std::filesystem::path>&)> dialog_callback;
        bool dialog_success = false;
        std::list<std::filesystem::path> dialog_paths;

        std::function<void(bool, const std::string&)> import_callback;
        bool import_success = false;
        std::string import_message;

        {
            std::lock_guard lock{ g_java_answer_mutex };
            if (g_dialog_answered) {
                dialog_callback = std::move(g_dialog_callback);
                g_dialog_callback = nullptr;
                dialog_success = g_dialog_success;
                dialog_paths = std::move(g_dialog_paths);
                g_dialog_paths.clear();
                g_dialog_answered = false;
            }
            if (g_import_answered) {
                import_callback = std::move(g_import_callback);
                g_import_callback = nullptr;
                import_success = g_import_success;
                import_message = std::move(g_import_message);
                g_import_message.clear();
                g_import_answered = false;
            }
        }

        // Callbacks run outside the lock and on this (render) thread, which is
        // what makes them safe to touch the UI from -- the same thread the
        // desktop file dialog's callback runs on.
        if (dialog_callback) {
            dialog_callback(dialog_success, dialog_paths);
        }
        if (import_callback) {
            import_callback(import_success, import_message);
        }
    }

    // -------------------------------------------- optional user-supplied driver

    bool android_gpu_driver_supported() {
#if defined(GOEMON_CUSTOM_VULKAN_DRIVER)
        return g_activity != nullptr;
#else
        return false;
#endif
    }

    GpuDriverState android_gpu_driver_state() {
        GpuDriverState state;
        std::string raw = call_java_string(g_mid_gpu_state, "gpuDriverStateJson");
        if (raw.empty()) {
            return state;
        }
        nlohmann::json json = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
        if (json.is_discarded() || !json.is_object()) {
            LOGE("custom driver: could not parse the state Java handed back");
            return state;
        }
        state.active_id = json.value("activeId", std::string{});
        state.device = json.value("device", std::string{});
        state.loader = json.value("loader", std::string{});
        state.needs_confirmation = json.value("needsConfirmation", false);
        if (json.contains("drivers") && json["drivers"].is_array()) {
            for (const auto& entry : json["drivers"]) {
                if (!entry.is_object()) {
                    continue;
                }
                state.drivers.push_back(GpuDriverEntry{
                    entry.value("id", std::string{}),
                    entry.value("name", std::string{}),
                    entry.value("detail", std::string{}),
                });
            }
        }
        return state;
    }

    void android_gpu_driver_select(const std::string& id) {
        call_java_void_string(g_mid_gpu_select, id, "gpuDriverSelect");
    }

    void android_gpu_driver_remove(const std::string& id) {
        call_java_void_string(g_mid_gpu_remove, id, "gpuDriverRemove");
    }

    void android_gpu_driver_confirm(const std::string& id) {
        call_java_void_string(g_mid_gpu_confirm, id, "gpuDriverConfirm");
    }

    void android_gpu_driver_note_survived() {
        call_java_void(g_mid_gpu_note_survived, "gpuDriverNoteSurvived");
    }

    void android_gpu_driver_request_import(std::function<void(bool, const std::string&)> callback) {
        bool refuse = false;
        {
            std::lock_guard lock{ g_java_answer_mutex };
            refuse = (g_import_callback != nullptr) || (g_activity == nullptr);
            if (!refuse) {
                g_import_callback = std::move(callback);
                g_import_answered = false;
            }
        }
        if (refuse) {
            callback(false, {});
            return;
        }

        JNIEnv* env = java_env();
        if (env != nullptr && g_mid_request_driver_import != nullptr) {
            env->CallVoidMethod(g_activity, g_mid_request_driver_import);
            clear_java_exception(env, "requestDriverImport");
        }
    }
}

extern "C" {

JNIEXPORT void JNICALL
Java_com_goemon64_recomp_MainActivity_nativeInit(JNIEnv* env, jobject thiz, jstring dataPath, jboolean autostart) {
    // Resolve everything the game will later want to call on the activity. This
    // has to happen here, on the UI thread: the render thread that makes those
    // calls was never entered from Java, so a class lookup from it would search
    // the system class loader and miss every class in this app. See the notes on
    // the cached handles above.
    g_activity = env->NewGlobalRef(thiz);
    jclass local_class = env->GetObjectClass(thiz);
    g_activity_class = static_cast<jclass>(env->NewGlobalRef(local_class));
    env->DeleteLocalRef(local_class);
    g_mid_request_open_document = env->GetMethodID(g_activity_class, "requestOpenDocument", "(Z)V");
    g_mid_request_driver_import = env->GetMethodID(g_activity_class, "requestDriverImport", "()V");
    g_mid_gpu_state = env->GetMethodID(g_activity_class, "gpuDriverStateJson", "()Ljava/lang/String;");
    g_mid_gpu_select = env->GetMethodID(g_activity_class, "gpuDriverSelect", "(Ljava/lang/String;)V");
    g_mid_gpu_remove = env->GetMethodID(g_activity_class, "gpuDriverRemove", "(Ljava/lang/String;)V");
    g_mid_gpu_confirm = env->GetMethodID(g_activity_class, "gpuDriverConfirm", "(Ljava/lang/String;)V");
    g_mid_gpu_note_survived = env->GetMethodID(g_activity_class, "gpuDriverNoteSurvived", "()V");
    // A missing method is a rename that got away, and it would otherwise surface
    // as a feature that silently does nothing.
    clear_java_exception(env, "resolving MainActivity methods");

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
// UnsatisfiedLinkError. Passing an empty driverName means "use the system
// driver", so even a custom-driver build behaves normally until a user selects
// something in the game's GPU driver settings.
//
// The driver and its name come from the Java side rather than being discovered
// here: GpuDriverStore has already parsed the package's meta.json for the
// library name, which is the only thing that reliably identifies it, and it
// supports several installed drivers at once.
//
// Returns a CustomDriverStatus for the driver UI to report.
JNIEXPORT jint JNICALL
Java_com_goemon64_recomp_MainActivity_nativeInitCustomDriver(JNIEnv* env, jobject /*thiz*/,
                                                             jstring hookLibDir, jstring driverDir,
                                                             jstring driverName, jstring tmpLibDir,
                                                             jstring stateDir) {
#if defined(GOEMON_CUSTOM_VULKAN_DRIVER)
    auto to_string = [env](jstring value) -> std::string {
        if (value == nullptr) {
            return {};
        }
        const char* chars = env->GetStringUTFChars(value, nullptr);
        std::string out = (chars != nullptr) ? chars : "";
        env->ReleaseStringUTFChars(value, chars);
        return out;
    };

    const std::string hook_dir = to_string(hookLibDir);
    const std::string driver_name = to_string(driverName);
    const std::string tmp_dir = to_string(tmpLibDir);
    g_driver_state_dir = to_string(stateDir);

    // MUST end in a separator: adrenotools_open_libvulkan builds the driver path
    // as customDriverDir + customDriverName with nothing between them, so a
    // directory without the trailing slash makes its stat() miss and the whole
    // call return null with no diagnostic (driver.cpp:45 in the vendored copy).
    std::string driver_dir = to_string(driverDir);
    if (!driver_dir.empty() && driver_dir.back() != '/') {
        driver_dir.push_back('/');
    }

    // Reset both results up front. Otherwise a launch that does not reach the
    // renderer leaves the previous launch's values on screen, which is exactly
    // the situation the user is trying to diagnose.
    write_driver_state("last_device", "");

    if (driver_name.empty()) {
        LOGI("custom driver: none selected, using the system Vulkan driver");
        set_driver_status(CustomDriverStatus::NotAttempted);
        return static_cast<jint>(CustomDriverStatus::NotAttempted);
    }
    if (driver_dir.empty() || hook_dir.empty()) {
        LOGE("custom driver: missing driver dir or hook dir, using the system Vulkan driver");
        set_driver_status(CustomDriverStatus::BadArguments);
        return static_cast<jint>(CustomDriverStatus::BadArguments);
    }

    // tmpLibDir is only consulted below API 29, where libadrenotools patches the
    // hook libraries on disk because memfd may be unavailable. Passing null there
    // makes it attempt memfd and return null if the kernel lacks it -- and minSdk
    // is 28, so a device at exactly 28 would silently fall through to the system
    // driver. Passing a writable directory removes that failure mode. It is
    // ignored on API 29 and above, so it costs nothing to always supply it.
    //
    // hookLibDir MUST be nativeLibraryDir and the APK must use legacy jniLibs
    // packaging, or the hook silently does nothing (see adrenotools/driver.h).
    void* libvulkan = adrenotools_open_libvulkan(
        RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
        tmp_dir.empty() ? nullptr : tmp_dir.c_str(),
        hook_dir.c_str(),
        driver_dir.c_str(),
        driver_name.c_str(),
        /*fileRedirectDir=*/nullptr,
        /*userMappingHandle=*/nullptr);
    if (libvulkan == nullptr) {
        // It reports no reason, so log everything a reader would otherwise have to
        // guess at. Every one of its early exits is a silent `return nullptr`.
        LOGE("custom driver: adrenotools_open_libvulkan failed -- driver '%s' dir '%s' hooks '%s' tmp '%s'",
             driver_name.c_str(), driver_dir.c_str(), hook_dir.c_str(), tmp_dir.c_str());
        set_driver_status(CustomDriverStatus::OpenFailed);
        return static_cast<jint>(CustomDriverStatus::OpenFailed);
    }

    auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(libvulkan, "vkGetInstanceProcAddr"));
    if (get_instance_proc_addr == nullptr) {
        LOGE("custom driver: vkGetInstanceProcAddr missing from %s (%s)", driver_name.c_str(), dlerror());
        set_driver_status(CustomDriverStatus::NoProcAddr);
        return static_cast<jint>(CustomDriverStatus::NoProcAddr);
    }

    plume::SetCustomVulkanLoader(get_instance_proc_addr);
    // Deliberately hedged: a non-null handle does NOT prove the custom driver is
    // in use. adrenotools can succeed here and still fall back to the system
    // driver, or enumerate zero devices, if the hook libraries were not found.
    // The device name recorded by report_render_device() below is the authority,
    // and it is what the driver settings show the user.
    LOGI("custom driver: requested %s via libadrenotools (hooks: %s) -- confirm with the reported device",
         driver_name.c_str(), hook_dir.c_str());
    set_driver_status(CustomDriverStatus::Requested);
    return static_cast<jint>(CustomDriverStatus::Requested);
#else
    (void)env;
    (void)hookLibDir;
    (void)driverDir;
    (void)driverName;
    (void)tmpLibDir;
    (void)stateDir;
    return 0;
#endif
}

JNIEXPORT void JNICALL
Java_com_goemon64_recomp_MainActivity_nativeDestroy(JNIEnv* /*env*/, jobject /*thiz*/) {
    LOGI("nativeDestroy");
}

// The user has finished with (or dismissed) the document picker native code
// asked for. Called on a Java background thread once the picked documents have
// been copied somewhere native code can open; the paths are those copies.
//
// This only parks the answer. Running the callback here would run UI code on a
// Java worker thread; android_pump_ui_callbacks() picks it up on the render
// thread instead.
JNIEXPORT void JNICALL
Java_com_goemon64_recomp_MainActivity_nativeOnFileDialogResult(JNIEnv* env, jobject /*thiz*/,
                                                               jboolean success, jobjectArray paths) {
    std::list<std::filesystem::path> parsed;
    if (paths != nullptr) {
        const jsize count = env->GetArrayLength(paths);
        for (jsize i = 0; i < count; i++) {
            auto value = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
            if (value != nullptr) {
                parsed.emplace_back(to_string(env, value));
                env->DeleteLocalRef(value);
            }
        }
    }

    std::lock_guard lock{ g_java_answer_mutex };
    g_dialog_success = (success == JNI_TRUE);
    g_dialog_paths = std::move(parsed);
    g_dialog_answered = true;
}

// Outcome of a GPU driver import: the driver's name on success, or the reason it
// was rejected. Parked for the render thread, as above.
JNIEXPORT void JNICALL
Java_com_goemon64_recomp_MainActivity_nativeOnDriverImportResult(JNIEnv* env, jobject /*thiz*/,
                                                                 jboolean success, jstring message) {
    std::string parsed = to_string(env, message);

    std::lock_guard lock{ g_java_answer_mutex };
    g_import_success = (success == JNI_TRUE);
    g_import_message = std::move(parsed);
    g_import_answered = true;
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
