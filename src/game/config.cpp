#include "goemon_config.h"
#include "recomp_input.h"
#include "goemon_sound.h"
#include "goemon_render.h"
#include "goemon_support.h"
#include "ultramodern/config.hpp"
#include "librecomp/files.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>

#if defined(_WIN32)
#include <Shlobj.h>
#elif defined(__linux__)
#include <unistd.h>
#include <pwd.h>
#elif defined(__APPLE__)
#include "apple/rt64_apple.h"
#endif

constexpr std::u8string_view general_filename = u8"general.json";
constexpr std::u8string_view graphics_filename = u8"graphics.json";
constexpr std::u8string_view controls_filename = u8"controls.json";
constexpr std::u8string_view sound_filename = u8"sound.json";

// Resolution default is device-class-dependent. Desktop uses Auto
// (WindowIntegerScale) which tracks the display. On Android that adapts to the
// panel and can land at 8x (~64x native pixels) on higher-res handhelds/TVs, which
// the per-pass GPU compute across ~20 heavy-menu composites cannot afford (device
// profiling 2026-07-19: 8x=30 FPS vs 4x=52 FPS on the Adreno 650 at the same
// clock). Pin 4x (~16x native) on Android — the measured sweet spot on the Retroid
// Pocket 5 reference target — instead of an unbounded Auto. Non-destructive:
// from_or_default applies this only when the JSON key is absent, so existing users
// keep their saved value.
#if defined(__ANDROID__)
constexpr auto res_default            = ultramodern::renderer::Resolution::Original4x;
#else
constexpr auto res_default            = ultramodern::renderer::Resolution::Auto;
#endif
constexpr auto hr_default             = ultramodern::renderer::HUDRatioMode::Clamp16x9;
constexpr auto api_default            = ultramodern::renderer::GraphicsApi::Auto;
constexpr auto ar_default             = ultramodern::renderer::AspectRatio::Expand;
// MSAA default is device-class-dependent. On handheld GPUs (Adreno 650 on the
// Retroid Pocket 5, the reference Android target) MSAA is paid per-sample on EVERY
// framebuffer pair, and heavy multi-pass menu screens submit ~20 render-target
// composites per frame — so 2x MSAA is a ~20% frame-cost multiplier stacked on top
// of the already-supersampled Auto resolution, for little visible gain at that
// scale. Device profiling (docs/android-profiling-results.md, 2026-07-19) traced
// the ~14 FPS Select-Adventure-Diary screen to exactly this pixel/sample cost.
// Default MSAA off on Android; desktop keeps 2x. Non-destructive: from_or_default
// only applies this when the JSON key is ABSENT, so existing users keep their choice.
#if defined(__ANDROID__)
constexpr auto msaa_default           = ultramodern::renderer::Antialiasing::None;
#else
constexpr auto msaa_default           = ultramodern::renderer::Antialiasing::MSAA2X;
#endif
constexpr auto rr_default             = ultramodern::renderer::RefreshRate::Display;
constexpr auto hpfb_default           = ultramodern::renderer::HighPrecisionFramebuffer::Auto;
constexpr int ds_default              = 1;
constexpr int rr_manual_default       = 60;
constexpr bool developer_mode_default = false;

static bool is_steam_deck = false;

ultramodern::renderer::WindowMode wm_default() {
    return is_steam_deck ? ultramodern::renderer::WindowMode::Fullscreen : ultramodern::renderer::WindowMode::Windowed;
}

#ifdef __gnu_linux__
void detect_steam_deck() {
    // Check if the board vendor is Valve.
    std::ifstream board_vendor_file("/sys/devices/virtual/dmi/id/board_vendor");
    std::string line;
    if (std::getline(board_vendor_file, line).good() && line == "Valve") {
        is_steam_deck = true;
        return;
    }

    // Check if the SteamDeck variable is set to 1.
    const char* steam_deck_env = getenv("SteamDeck");
    if (steam_deck_env != nullptr && std::string{steam_deck_env} == "1") {
        is_steam_deck = true;
        return;
    }

    is_steam_deck = false;
    return;
}
#else
void detect_steam_deck() { is_steam_deck = false; }
#endif

template <typename T>
T from_or_default(const json& j, const std::string& key, T default_value) {
    T ret;
    auto find_it = j.find(key);
    if (find_it != j.end()) {
        find_it->get_to(ret);
    }
    else {
        ret = default_value;
    }

    return ret;
}

template <typename T>
void call_if_key_exists(void (*func)(T), const json& j, const std::string& key) {
    auto find_it = j.find(key);
    if (find_it != j.end()) {
        T val;
        find_it->get_to(val);
        func(val);
    }
}

namespace ultramodern {
    void to_json(json& j, const renderer::GraphicsConfig& config) {
        j = json{
            {"res_option",      config.res_option},
            {"wm_option",       config.wm_option},
            {"hr_option",       config.hr_option},
            {"api_option",      config.api_option},
            {"ds_option",       config.ds_option},
            {"ar_option",       config.ar_option},
            {"msaa_option",     config.msaa_option},
            {"rr_option",       config.rr_option},
            {"hpfb_option",     config.hpfb_option},
            {"rr_manual_value", config.rr_manual_value},
            {"developer_mode",  config.developer_mode},
        };
    }

    void from_json(const json& j, renderer::GraphicsConfig& config) {
        config.res_option       = from_or_default(j, "res_option",      res_default);
        config.wm_option        = from_or_default(j, "wm_option",       wm_default());
        config.hr_option        = from_or_default(j, "hr_option",       hr_default);
        config.api_option       = from_or_default(j, "api_option",      api_default);
        config.ds_option        = from_or_default(j, "ds_option",       ds_default);
        config.ar_option        = from_or_default(j, "ar_option",       ar_default);
        config.msaa_option      = from_or_default(j, "msaa_option",     msaa_default);
        config.rr_option        = from_or_default(j, "rr_option",       rr_default);
        config.hpfb_option      = from_or_default(j, "hpfb_option",     hpfb_default);
        config.rr_manual_value  = from_or_default(j, "rr_manual_value", rr_manual_default);
        config.developer_mode   = from_or_default(j, "developer_mode",  developer_mode_default);
    }
}

namespace recomp {
    void to_json(json& j, const InputField& field) {
        j = json{ {"input_type", field.input_type}, {"input_id", field.input_id} };
    }

    void from_json(const json& j, InputField& field) {
        j.at("input_type").get_to(field.input_type);
        j.at("input_id").get_to(field.input_id);
    }
}

std::filesystem::path goemon64::get_app_folder_path() {
   // directly check for portable.txt (windows and native linux binary)
   if (std::filesystem::exists("portable.txt")) {
       return std::filesystem::current_path();
   }

#if defined(__APPLE__)
   // Check for portable file in the directory containing the app bundle.
   const auto app_bundle_path = goemon64::get_bundle_directory().parent_path();
   if (std::filesystem::exists(app_bundle_path / "portable.txt")) {
       return app_bundle_path;
   }
#endif

   std::filesystem::path recomp_dir{};

#if defined(_WIN32)
   // Deduce local app data path.
   PWSTR known_path = NULL;
   HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &known_path);
   if (result == S_OK) {
       recomp_dir = std::filesystem::path{known_path} / goemon64::program_id;
   }

   CoTaskMemFree(known_path);
#elif defined(__linux__) || defined(__APPLE__)
   // check for APP_FOLDER_PATH env var
   if (getenv("APP_FOLDER_PATH") != nullptr) {
       return std::filesystem::path{getenv("APP_FOLDER_PATH")};
   }

#if defined(__APPLE__)
   const auto supportdir = goemon64::get_application_support_directory();
   if (supportdir) {
       return *supportdir / goemon64::program_id;
   }
#endif

   const char *homedir;

   if ((homedir = getenv("HOME")) == nullptr) {
    #if defined(__linux__)
       homedir = getpwuid(getuid())->pw_dir;
    #elif defined(__APPLE__)
        homedir = GetHomeDirectory();
    #endif
   }

   if (homedir != nullptr) {
       recomp_dir = std::filesystem::path{homedir} / (std::u8string{u8".config/"} + std::u8string{goemon64::program_id});
   }
#endif

    return recomp_dir;
}

bool read_json(std::ifstream input_file, nlohmann::json& json_out) {
    if (!input_file.good()) {
        return false;
    }

    try {
        input_file >> json_out;
    }
    catch (nlohmann::json::parse_error&) {
        return false;
    }
    return true;
}

bool read_json_with_backups(const std::filesystem::path& path, nlohmann::json& json_out) {
    // Try reading and parsing the base file.
    if (read_json(std::ifstream{path}, json_out)) {
        return true;
    }

    // Try reading and parsing the backup file.
    if (read_json(recomp::open_input_backup_file(path), json_out)) {
        return true;
    }

    // Both reads failed.
    return false;
}

bool save_json_with_backups(const std::filesystem::path& path, const nlohmann::json& json_data) {
    {
        std::ofstream output_file = recomp::open_output_file_with_backup(path);
        if (!output_file.good()) {
            return false;
        }

        output_file << std::setw(4) << json_data;

        // If the write failed (e.g. disk full), do NOT promote the temp file
        // over the real config — leave the existing good file in place.
        output_file.flush();
        if (!output_file.good()) {
            return false;
        }
    }
    return recomp::finalize_output_file_with_backup(path);
}

bool save_general_config(const std::filesystem::path& path) {
    nlohmann::json config_json{};

    goemon64::to_json(config_json["targeting_mode"], goemon64::get_targeting_mode());
    recomp::to_json(config_json["background_input_mode"], recomp::get_background_input_mode());
    config_json["rumble_strength"] = recomp::get_rumble_strength();
    config_json["gyro_sensitivity"] = recomp::get_gyro_sensitivity();
    config_json["mouse_sensitivity"] = recomp::get_mouse_sensitivity();
    config_json["joystick_deadzone"] = recomp::get_joystick_deadzone();
    config_json["autosave_mode"] = goemon64::get_autosave_mode();
    config_json["camera_invert_mode"] = goemon64::get_camera_invert_mode();
    config_json["analog_cam_mode"] = goemon64::get_analog_cam_mode();
    config_json["analog_camera_invert_mode"] = goemon64::get_analog_camera_invert_mode();
    config_json["analog_cam_sensitivity_x"] = goemon64::get_analog_cam_sensitivity_x();
    config_json["analog_cam_sensitivity_y"] = goemon64::get_analog_cam_sensitivity_y();
    config_json["swap_while_moving_mode"] = goemon64::get_swap_while_moving_mode();
    config_json["attack_while_moving_mode"] = goemon64::get_attack_while_moving_mode();
    config_json["infinite_health_mode"] = goemon64::get_infinite_health_mode();
    config_json["infinite_money_mode"] = goemon64::get_infinite_money_mode();
    config_json["infinite_lives_mode"] = goemon64::get_infinite_lives_mode();
    config_json["debug_mode"] = goemon64::get_debug_mode_enabled();

    return save_json_with_backups(path, config_json);
}

void set_general_settings_from_json(const nlohmann::json& config_json) {
    goemon64::set_targeting_mode(from_or_default(config_json, "targeting_mode", goemon64::TargetingMode::Switch));
    recomp::set_background_input_mode(from_or_default(config_json, "background_input_mode", recomp::BackgroundInputMode::On));
    recomp::set_rumble_strength(from_or_default(config_json, "rumble_strength", 25));
    recomp::set_gyro_sensitivity(from_or_default(config_json, "gyro_sensitivity", 50));
    recomp::set_mouse_sensitivity(from_or_default(config_json, "mouse_sensitivity", is_steam_deck ? 50 : 0));
    recomp::set_joystick_deadzone(from_or_default(config_json, "joystick_deadzone", 5));
    goemon64::set_autosave_mode(from_or_default(config_json, "autosave_mode", goemon64::AutosaveMode::Off));
    goemon64::set_camera_invert_mode(from_or_default(config_json, "camera_invert_mode", goemon64::CameraInvertMode::InvertY));
    goemon64::set_analog_cam_mode(from_or_default(config_json, "analog_cam_mode", goemon64::AnalogCamMode::Off));
    goemon64::set_analog_camera_invert_mode(from_or_default(config_json, "analog_camera_invert_mode", goemon64::CameraInvertMode::InvertNone));
    goemon64::set_analog_cam_sensitivity_x(from_or_default(config_json, "analog_cam_sensitivity_x", 50));
    goemon64::set_analog_cam_sensitivity_y(from_or_default(config_json, "analog_cam_sensitivity_y", 50));
    goemon64::set_swap_while_moving_mode(from_or_default(config_json, "swap_while_moving_mode", goemon64::SwapWhileMovingMode::Off));
    goemon64::set_attack_while_moving_mode(from_or_default(config_json, "attack_while_moving_mode", goemon64::AttackWhileMovingMode::Off));
    goemon64::set_infinite_health_mode(from_or_default(config_json, "infinite_health_mode", goemon64::CheatMode::Off));
    goemon64::set_infinite_money_mode(from_or_default(config_json, "infinite_money_mode", goemon64::CheatMode::Off));
    goemon64::set_infinite_lives_mode(from_or_default(config_json, "infinite_lives_mode", goemon64::CheatMode::Off));
    goemon64::set_debug_mode_enabled(from_or_default(config_json, "debug_mode", false));
}

bool load_general_config(const std::filesystem::path& path) {
    nlohmann::json config_json{};
    if (!read_json_with_backups(path, config_json)) {
        return false;
    }

    try {
        set_general_settings_from_json(config_json);
    }
    catch (const nlohmann::json::exception&) {
        // read_json only catches parse_error; a parseable-but-wrong-typed value
        // (e.g. "rumble_strength": "25", a null binding, a missing input_id)
        // throws type_error/out_of_range from the get_to()/at() calls during
        // deserialization. Uncaught, that propagates to main -> std::terminate
        // pre-window (a crash loop, since .bak is a byte-identical rotation).
        // Treat it like a corrupt file: return false so load_config() falls
        // through to its reset-to-defaults + save path.
        return false;
    }
    return true;
}

void assign_mapping(recomp::InputDevice device, recomp::GameInput input, const std::vector<recomp::InputField>& value) {
    for (size_t binding_index = 0; binding_index < std::min(value.size(), recomp::bindings_per_input); binding_index++) {
        recomp::set_input_binding(input, binding_index, device, value[binding_index]);
    }
};

// same as assign_mapping, except will clear unassigned bindings if not in value
void assign_mapping_complete(recomp::InputDevice device, recomp::GameInput input, const std::vector<recomp::InputField>& value) {
    for (size_t binding_index = 0; binding_index < recomp::bindings_per_input; binding_index++) {
        if (binding_index >= value.size()) {
            recomp::set_input_binding(input, binding_index, device, recomp::InputField{});
        } else {
            recomp::set_input_binding(input, binding_index, device, value[binding_index]);
        }
    }
};

void assign_all_mappings(recomp::InputDevice device, const recomp::DefaultN64Mappings& values) {
    assign_mapping_complete(device, recomp::GameInput::A, values.a);
    assign_mapping_complete(device, recomp::GameInput::B, values.b);
    assign_mapping_complete(device, recomp::GameInput::Z, values.z);
    assign_mapping_complete(device, recomp::GameInput::START, values.start);
    assign_mapping_complete(device, recomp::GameInput::DPAD_UP, values.dpad_up);
    assign_mapping_complete(device, recomp::GameInput::DPAD_DOWN, values.dpad_down);
    assign_mapping_complete(device, recomp::GameInput::DPAD_LEFT, values.dpad_left);
    assign_mapping_complete(device, recomp::GameInput::DPAD_RIGHT, values.dpad_right);
    assign_mapping_complete(device, recomp::GameInput::L, values.l);
    assign_mapping_complete(device, recomp::GameInput::R, values.r);
    assign_mapping_complete(device, recomp::GameInput::C_UP, values.c_up);
    assign_mapping_complete(device, recomp::GameInput::C_DOWN, values.c_down);
    assign_mapping_complete(device, recomp::GameInput::C_LEFT, values.c_left);
    assign_mapping_complete(device, recomp::GameInput::C_RIGHT, values.c_right);

    assign_mapping_complete(device, recomp::GameInput::X_AXIS_NEG, values.analog_left);
    assign_mapping_complete(device, recomp::GameInput::X_AXIS_POS, values.analog_right);
    assign_mapping_complete(device, recomp::GameInput::Y_AXIS_NEG, values.analog_down);
    assign_mapping_complete(device, recomp::GameInput::Y_AXIS_POS, values.analog_up);

    assign_mapping_complete(device, recomp::GameInput::TOGGLE_MENU, values.toggle_menu);
    assign_mapping_complete(device, recomp::GameInput::ACCEPT_MENU, values.accept_menu);
    assign_mapping_complete(device, recomp::GameInput::APPLY_MENU, values.apply_menu);
};

void goemon64::reset_input_bindings() {
    assign_all_mappings(recomp::InputDevice::Keyboard, recomp::default_n64_keyboard_mappings);
    assign_all_mappings(recomp::InputDevice::Controller, recomp::default_n64_controller_mappings);
}

void goemon64::reset_cont_input_bindings() {
    assign_all_mappings(recomp::InputDevice::Controller, recomp::default_n64_controller_mappings);
}

void goemon64::reset_kb_input_bindings() {
    assign_all_mappings(recomp::InputDevice::Keyboard, recomp::default_n64_keyboard_mappings);
}

void goemon64::reset_single_input_binding(recomp::InputDevice device, recomp::GameInput input) {
    assign_mapping_complete(
        device,
        input,
        recomp::get_default_mapping_for_input(
            device == recomp::InputDevice::Keyboard ?
                recomp::default_n64_keyboard_mappings :
                recomp::default_n64_controller_mappings,
            input
        )
    );
}

void reset_graphics_options() {
    ultramodern::renderer::GraphicsConfig new_config{};
    new_config.res_option = res_default;
    new_config.wm_option = wm_default();
    new_config.hr_option = hr_default;
    new_config.ds_option = ds_default;
    new_config.ar_option = ar_default;
    new_config.msaa_option = msaa_default;
    new_config.rr_option = rr_default;
    new_config.hpfb_option = hpfb_default;
    new_config.rr_manual_value = rr_manual_default;
    new_config.developer_mode = developer_mode_default;
    ultramodern::renderer::set_graphics_config(new_config);
}

bool save_graphics_config(const std::filesystem::path& path) {
    nlohmann::json config_json{};
    ultramodern::to_json(config_json, ultramodern::renderer::get_graphics_config());
    return save_json_with_backups(path, config_json);
}

bool load_graphics_config(const std::filesystem::path& path) {
    nlohmann::json config_json{};
    if (!read_json_with_backups(path, config_json)) {
        return false;
    }

    ultramodern::renderer::GraphicsConfig new_config{};
    try {
        ultramodern::from_json(config_json, new_config);
    }
    catch (const nlohmann::json::exception&) {
        // Wrong-typed value -> type_error; recover via load_config's reset path
        // instead of terminating. See load_general_config for the full rationale.
        return false;
    }
    ultramodern::renderer::set_graphics_config(new_config);
    return true;
}

void add_input_bindings(nlohmann::json& out, recomp::GameInput input, recomp::InputDevice device) {
    const std::string& input_name = recomp::get_input_enum_name(input);
    nlohmann::json& out_array = out[input_name];
    out_array = nlohmann::json::array();
    for (size_t binding_index = 0; binding_index < recomp::bindings_per_input; binding_index++) {
        out_array[binding_index] = recomp::get_input_binding(input, binding_index, device);
    }
};

bool save_controls_config(const std::filesystem::path& path) {
    nlohmann::json config_json{};

    config_json["keyboard"] = {};
    config_json["controller"] = {};

    for (size_t i = 0; i < recomp::get_num_inputs(); i++) {
        recomp::GameInput cur_input = static_cast<recomp::GameInput>(i);

        add_input_bindings(config_json["keyboard"], cur_input, recomp::InputDevice::Keyboard);
        add_input_bindings(config_json["controller"], cur_input, recomp::InputDevice::Controller);
    }

    return save_json_with_backups(path, config_json);
}

bool load_input_device_from_json(const nlohmann::json& config_json, recomp::InputDevice device, const std::string& key) {
    // Check if the json object for the given key exists.
    auto find_it = config_json.find(key);
    if (find_it == config_json.end()) {
        return false;
    }

    const nlohmann::json& mappings_json = *find_it;

    for (size_t i = 0; i < recomp::get_num_inputs(); i++) {
        recomp::GameInput cur_input = static_cast<recomp::GameInput>(i);
        const std::string& input_name = recomp::get_input_enum_name(cur_input);

        // Check if the json object for the given input exists and that it's an array.
        auto find_input_it = mappings_json.find(input_name);
        if (find_input_it == mappings_json.end() || !find_input_it->is_array()) {
            assign_mapping(
                device,
                cur_input,
                recomp::get_default_mapping_for_input(
                    device == recomp::InputDevice::Keyboard ?
                    recomp::default_n64_keyboard_mappings :
                    recomp::default_n64_controller_mappings,
                    cur_input
                )
            );
            continue;
        }
        const nlohmann::json& input_json = *find_input_it;

        // Deserialize all the bindings from the json array (up to the max number of bindings per input).
        for (size_t binding_index = 0; binding_index < std::min(recomp::bindings_per_input, input_json.size()); binding_index++) {
            recomp::InputField cur_field{};
            recomp::from_json(input_json[binding_index], cur_field);
            recomp::set_input_binding(cur_input, binding_index, device, cur_field);
        }
    }

    return true;
}

bool load_controls_config(const std::filesystem::path& path) {
    nlohmann::json config_json{};
    if (!read_json_with_backups(path, config_json)) {
        return false;
    }

    try {
        if (!load_input_device_from_json(config_json, recomp::InputDevice::Keyboard, "keyboard")) {
            assign_all_mappings(recomp::InputDevice::Keyboard, recomp::default_n64_keyboard_mappings);
        }

        if (!load_input_device_from_json(config_json, recomp::InputDevice::Controller, "controller")) {
            assign_all_mappings(recomp::InputDevice::Controller, recomp::default_n64_controller_mappings);
        }
    }
    catch (const nlohmann::json::exception&) {
        // A malformed binding (wrong-typed/missing input_type/input_id) throws
        // from recomp::from_json. Recover via load_config's reset path (resets
        // all bindings to defaults). See load_general_config for the rationale.
        return false;
    }
    return true;
}

bool save_sound_config(const std::filesystem::path& path) {
    nlohmann::json config_json{};

    config_json["main_volume"] = goemon64::get_main_volume();
    config_json["bgm_volume"] = goemon64::get_bgm_volume();
    config_json["se_volume"] = goemon64::get_se_volume();

    return save_json_with_backups(path, config_json);
}

bool load_sound_config(const std::filesystem::path& path) {
    nlohmann::json config_json{};
    if (!read_json_with_backups(path, config_json)) {
        return false;
    }

    try {
        goemon64::reset_sound_settings();
        call_if_key_exists(goemon64::set_main_volume, config_json, "main_volume");
        call_if_key_exists(goemon64::set_bgm_volume, config_json, "bgm_volume");
        call_if_key_exists(goemon64::set_se_volume, config_json, "se_volume");
    }
    catch (const nlohmann::json::exception&) {
        // Wrong-typed volume -> type_error; recover via load_config's reset path.
        // See load_general_config for the full rationale.
        return false;
    }
    return true;
}

void goemon64::load_config() {
    detect_steam_deck();

    std::filesystem::path recomp_dir = goemon64::get_app_folder_path();
    std::filesystem::path general_path = recomp_dir / general_filename;
    std::filesystem::path graphics_path = recomp_dir / graphics_filename;
    std::filesystem::path controls_path = recomp_dir / controls_filename;
    std::filesystem::path sound_path = recomp_dir / sound_filename;

    if (!recomp_dir.empty()) {
        // Non-throwing: if the dir can't be created (e.g. SD card vanished),
        // the load_* calls below fall back to defaults rather than aborting.
        std::error_code ec;
        std::filesystem::create_directories(recomp_dir, ec);
    }

    // TODO error handling for failing to save config files after resetting them.

    if (!load_general_config(general_path)) {
        // Set the general settings from an empty json to use defaults.
        set_general_settings_from_json({});
        save_general_config(general_path);
    }

    if (!load_graphics_config(graphics_path)) {
        reset_graphics_options();
        save_graphics_config(graphics_path);
    }

    if (!load_controls_config(controls_path)) {
        goemon64::reset_input_bindings();
        save_controls_config(controls_path);
    }

    if (!load_sound_config(sound_path)) {
        goemon64::reset_sound_settings();
        save_sound_config(sound_path);
    }
}

void goemon64::save_config() {
    std::filesystem::path recomp_dir = goemon64::get_app_folder_path();

    if (recomp_dir.empty()) {
        return;
    }

    // Non-throwing: closing the settings menu triggers this; a vanished SD
    // data dir must not SIGABRT. Bail if the dir can't be created.
    std::error_code ec;
    std::filesystem::create_directories(recomp_dir, ec);
    if (ec) {
        return;
    }

    // TODO error handling for failing to save config files.

    save_general_config(recomp_dir / general_filename);
    save_graphics_config(recomp_dir / graphics_filename);
    save_controls_config(recomp_dir / controls_filename);
    save_sound_config(recomp_dir / sound_filename);
}
