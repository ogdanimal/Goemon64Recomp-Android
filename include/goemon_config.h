#ifndef __GOEMON_CONFIG_H__
#define __GOEMON_CONFIG_H__

#include <filesystem>
#include <string_view>
#include "ultramodern/config.hpp"
#include "recomp_input.h"

namespace goemon64 {
    constexpr std::u8string_view program_id = u8"Goemon64Recompiled";
    constexpr std::string_view program_name = "Goemon 64: Recompiled";

    // TODO: Move loading configs to the runtime once we have a way to allow per-project customization.
    void load_config();
    void save_config();
    
    void reset_input_bindings();
    void reset_cont_input_bindings();
    void reset_kb_input_bindings();
    void reset_single_input_binding(recomp::InputDevice device, recomp::GameInput input);

    std::filesystem::path get_app_folder_path();
    
    bool get_debug_mode_enabled();
    void set_debug_mode_enabled(bool enabled);
    
    enum class AutosaveMode {
        On,
        Off,
        OptionCount
    };

    // Off listed FIRST: NLOHMANN_JSON_SERIALIZE_ENUM maps an unknown/wrong-typed
    // JSON value to the first pair's enum, so the fallback must be the safe
    // default (autosave stays Off on a corrupt/downgraded config, matching the
    // settled default). Order does not affect known-value (de)serialization.
    NLOHMANN_JSON_SERIALIZE_ENUM(goemon64::AutosaveMode, {
        {goemon64::AutosaveMode::Off, "Off"},
        {goemon64::AutosaveMode::On, "On"}
    });

    enum class TargetingMode {
        Switch,
        Hold,
        OptionCount
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(goemon64::TargetingMode, {
        {goemon64::TargetingMode::Switch, "Switch"},
        {goemon64::TargetingMode::Hold, "Hold"}
    });

    TargetingMode get_targeting_mode();
    void set_targeting_mode(TargetingMode mode);

    enum class CameraInvertMode {
        InvertNone,
        InvertX,
        InvertY,
        InvertBoth,
        OptionCount
    };

    // Unknown/wrong-typed values fall back to the first entry, InvertNone. NOTE:
    // this enum serves two settings with DIFFERENT defaults — analog_camera_invert
    // defaults InvertNone (matches the fallback), but camera_invert defaults
    // InvertY (does not). The mismatch cannot be fixed by reordering (one order
    // can't match both) and is benign: it's camera feel, not a fail-open toggle.
    NLOHMANN_JSON_SERIALIZE_ENUM(goemon64::CameraInvertMode, {
        {goemon64::CameraInvertMode::InvertNone, "InvertNone"},
        {goemon64::CameraInvertMode::InvertX, "InvertX"},
        {goemon64::CameraInvertMode::InvertY, "InvertY"},
        {goemon64::CameraInvertMode::InvertBoth, "InvertBoth"}
    });

    CameraInvertMode get_camera_invert_mode();
    void set_camera_invert_mode(CameraInvertMode mode);

    CameraInvertMode get_analog_camera_invert_mode();
    void set_analog_camera_invert_mode(CameraInvertMode mode);

    enum class AnalogCamMode {
        On,
        Off,
		OptionCount
    };

    // Off first so an unknown/wrong-typed value fails safe (see AutosaveMode).
    NLOHMANN_JSON_SERIALIZE_ENUM(goemon64::AnalogCamMode, {
        {goemon64::AnalogCamMode::Off, "Off"},
        {goemon64::AnalogCamMode::On, "On"}
    });

    AutosaveMode get_autosave_mode();
    void set_autosave_mode(AutosaveMode mode);

    AnalogCamMode get_analog_cam_mode();
    void set_analog_cam_mode(AnalogCamMode mode);

    // Analog-camera rotation sensitivity, 0-100 per axis. 50 = the tuned
    // default rate; the patch scales its base yaw/pitch rates by (value / 50),
    // so 100 is ~2x speed and 0 stops that axis.
    int get_analog_cam_sensitivity_x();
    void set_analog_cam_sensitivity_x(int sensitivity);
    int get_analog_cam_sensitivity_y();
    void set_analog_cam_sensitivity_y(int sensitivity);

    // Shared On/Off enum for the Cheats menu. One enum for every cheat toggle,
    // since they all have the same two states.
    enum class CheatMode {
        On,
        Off,
		OptionCount
    };

    // Off first so an unknown/wrong-typed value fails safe -- a corrupt config
    // must not silently enable a cheat (see AutosaveMode).
    NLOHMANN_JSON_SERIALIZE_ENUM(goemon64::CheatMode, {
        {goemon64::CheatMode::Off, "Off"},
        {goemon64::CheatMode::On, "On"}
    });

    CheatMode get_infinite_health_mode();
    void set_infinite_health_mode(CheatMode mode);

    CheatMode get_infinite_money_mode();
    void set_infinite_money_mode(CheatMode mode);

    CheatMode get_infinite_lives_mode();
    void set_infinite_lives_mode(CheatMode mode);

    // Allow starting a character swap while walking/running, instead of only
    // from an idle stance. Reuses the On/Off shape of the other toggles.
    enum class SwapWhileMovingMode {
        On,
        Off,
		OptionCount
    };

    // Off first so an unknown/wrong-typed value fails safe (see AutosaveMode).
    NLOHMANN_JSON_SERIALIZE_ENUM(goemon64::SwapWhileMovingMode, {
        {goemon64::SwapWhileMovingMode::Off, "Off"},
        {goemon64::SwapWhileMovingMode::On, "On"}
    });

    SwapWhileMovingMode get_swap_while_moving_mode();
    void set_swap_while_moving_mode(SwapWhileMovingMode mode);

    // Allow the player to keep moving during an attack (lunge in the direction
    // last run) instead of rooting in place. Reuses the On/Off toggle shape.
    enum class AttackWhileMovingMode {
        On,
        Off,
		OptionCount
    };

    // Off first so an unknown/wrong-typed value fails safe (see AutosaveMode).
    NLOHMANN_JSON_SERIALIZE_ENUM(goemon64::AttackWhileMovingMode, {
        {goemon64::AttackWhileMovingMode::Off, "Off"},
        {goemon64::AttackWhileMovingMode::On, "On"}
    });

    AttackWhileMovingMode get_attack_while_moving_mode();
    void set_attack_while_moving_mode(AttackWhileMovingMode mode);

    void open_quit_game_prompt();
    void open_restart_game_prompt();
};

#endif
