#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "ui_saves.h"

#include "recomp_ui.h"
#include "goemon_support.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

// The "Saves" settings category. See ui_saves.h for why it exists.
//
// TWO RULES SHAPE EVERYTHING BELOW, both about not destroying save data.
//
// 1. Importing is refused once the game has started. The running game holds the
//    save in emulated flash and writes that back out on its own schedule, so a
//    file dropped underneath it is either ignored or overwritten within
//    minutes -- and which of the two you get depends on timing, which is the
//    worst possible property for something handling save data. Before the game
//    starts there is no such copy: init_saving() reads the file, so an import
//    is simply what the game will find. Refusing outright is the only version
//    of this that is always right.
//
// 2. The save being replaced is copied aside first, and the import is staged
//    and renamed rather than written in place. Import is the one operation here
//    whose entire purpose is to overwrite the file the player cares most about.

extern std::vector<recomp::GameEntry> supported_games;

namespace {

struct SavesModelContext {
    // Shown as-is. Built by refresh_state() so the strings the document binds
    // never have to be assembled in the template.
    std::string save_path;
    std::string save_detail;
    std::string message;

    bool importable = false;      // no game running yet -> import is allowed
    bool has_save = false;
    bool game_started = false;    // cached, so tick_saves does no work per frame

    int focused_option = -1;
    Rml::DataModelHandle model_handle;
};

SavesModelContext saves_ctx;

const recomp::GameEntry& game_entry() {
    return supported_games.at(0);
}

std::filesystem::path save_file_path() {
    // Deliberately NOT ultramodern::get_save_file_path(): that reports the live
    // path and is empty until init_saving() runs, which by rule 1 above is
    // exactly when this feature is not allowed to act.
    return ultramodern::get_save_file_path_for(u8"", game_entry().game_id);
}

// Goemon's own save-block layout, from the save routine
// func_80214D58_5D0228 and the CRC helper func_80023A1C_2461C. The file is a
// 0x100-byte header block followed by SLOT_COUNT fixed-size slots, and each
// slot begins with a big-endian CRC over the 0x304 bytes of game data that
// follow it.
//
// The header is deliberately NOT checked. Only its first 0x10 bytes are real --
// the rest is uninitialised stack the write routine leaks -- and the game's own
// loader ignores a bad header CRC rather than rejecting the file, so requiring
// one here would be stricter than the game itself.
constexpr size_t save_header_size = 0x100;
constexpr size_t save_slot_size = 0x500;
constexpr size_t save_slot_data_len = 0x304;
constexpr size_t save_slot_count = 3;

// CRC-32/BZIP2: init 0xFFFFFFFF, polynomial 0x04C11DB7, MSB-first, no
// reflection, final complement. Confirmed against real save files rather than
// taken from the description -- slots verified byte-for-byte.
uint32_t save_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
        }
    }
    return ~crc;
}

// True if any save slot's stored CRC matches its own data.
//
// ANY rather than ALL, deliberately. A slot the player has never used is left
// as zeroes and cannot validate, so demanding all three would reject most real
// save files -- both of the ones this was developed against have an untouched
// third slot. One good slot is also exactly the condition for the file being
// worth importing at all: a file with none has nothing to load.
//
// The point of this check is that size alone does not distinguish a save from
// any other file that happens to be the same length, and a save from a
// different emulator can be the right size without being the right bytes.
// Such a file would otherwise import "successfully" and then not appear in the
// game, with the real save already moved aside.
bool contains_save_data(const std::vector<uint8_t>& bytes) {
    for (size_t slot = 0; slot < save_slot_count; slot++) {
        const size_t offset = save_header_size + slot * save_slot_size;
        if (offset + 4 + save_slot_data_len > bytes.size()) {
            break;
        }
        const uint8_t* block = bytes.data() + offset;
        const uint32_t stored = (static_cast<uint32_t>(block[0]) << 24)
                              | (static_cast<uint32_t>(block[1]) << 16)
                              | (static_cast<uint32_t>(block[2]) << 8)
                              |  static_cast<uint32_t>(block[3]);
        if (stored == save_crc32(block + 4, save_slot_data_len)) {
            return true;
        }
    }
    return false;
}

std::string human_size(uintmax_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " bytes";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    return buf;
}

void set_message(std::string text) {
    saves_ctx.message = std::move(text);
    if (saves_ctx.model_handle) {
        saves_ctx.model_handle.DirtyVariable("saves_message");
    }
}

void refresh_state() {
    const std::filesystem::path path = save_file_path();
    saves_ctx.save_path = path.string();

    std::error_code ec;
    saves_ctx.has_save = std::filesystem::is_regular_file(path, ec);
    if (saves_ctx.has_save) {
        const uintmax_t size = std::filesystem::file_size(path, ec);
        saves_ctx.save_detail = ec ? std::string{ "present" } : human_size(size);
    }
    else if (std::filesystem::exists(path, ec)) {
        // Something is there but it is not a regular file. Worth saying, because
        // it is the shape of problem that otherwise only shows up as saving
        // mysteriously failing later.
        saves_ctx.save_detail = "something that is not a save file is at this path";
    }
    else {
        saves_ctx.save_detail = "no save file yet";
    }

    saves_ctx.game_started = ultramodern::is_game_started();
    saves_ctx.importable = !saves_ctx.game_started;

    if (saves_ctx.model_handle) {
        saves_ctx.model_handle.DirtyAllVariables();
    }
}

// Copies `from` to `to` via a staged temp file in the destination's directory,
// so `to` is never observably half-written. Returns an empty string on success
// or a reason on failure.
std::string staged_copy(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::path temp = to;
    temp += ".importtemp";

    std::error_code ec;
    std::filesystem::copy_file(from, temp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return ec.message();
    }

    std::filesystem::rename(temp, to, ec);
    if (ec) {
        std::error_code cleanup_ec;
        std::filesystem::remove(temp, cleanup_ec);
        return ec.message();
    }
    return {};
}

struct ImportOutcome {
    bool ok = false;
    std::string message;
};

ImportOutcome import_save(const std::filesystem::path& source) {
    if (ultramodern::is_game_started()) {
        // Belt and braces: the button is disabled in this state, but the file
        // dialog is asynchronous, so the game could in principle have been
        // started between the picker opening and the file coming back.
        return { false, "The game started while the file was being chosen. Importing was cancelled." };
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        return { false, "That is not a file this app can read." };
    }

    const uintmax_t size = std::filesystem::file_size(source, ec);
    if (ec) {
        return { false, "Could not read that file: " + ec.message() };
    }

    // Size is the only check worth making. The save's own contents are the
    // game's business -- it validates its slots itself and reports an empty file
    // as an empty save -- but a file of the wrong size is definitely not one of
    // ours, and letting it through would replace a real save with something the
    // game cannot use.
    const size_t expected = recomp::get_save_size(game_entry().save_type);
    if (size != expected) {
        return { false, "That file is " + human_size(size) + ". A save file for this game is "
                        + human_size(expected) + ", so this is not one." };
    }

    // Read the header and slot region only: the slots live in the first 0x1000
    // bytes and nothing after them is structured.
    std::vector<uint8_t> head(save_header_size + save_slot_count * save_slot_size);
    {
        std::ifstream in(source, std::ios::binary);
        if (!in.good()) {
            return { false, "Could not open that file." };
        }
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        if (in.gcount() != static_cast<std::streamsize>(head.size())) {
            return { false, "Could not read that file." };
        }
    }
    if (!contains_save_data(head)) {
        return { false, "That file is the right size but does not hold Goemon save data. "
                        "A save from a different emulator can be the same size without being "
                        "the same format." };
    }

    const std::filesystem::path destination = save_file_path();
    const std::filesystem::path folder = destination.parent_path();

    // The specific failure that brought this feature about: a plain file sitting
    // where the saves folder should be, which is what copying a save in by hand
    // produces when the folder does not exist yet. Nothing can be created inside
    // it, and every save fails from then on.
    if (std::filesystem::exists(folder, ec) && !std::filesystem::is_directory(folder, ec)) {
        return { false, "A file is in the way of the save folder \"" + folder.string()
                        + "\". Delete that file and try again -- the folder will be recreated." };
    }

    std::filesystem::create_directories(folder, ec);
    if (ec) {
        return { false, "Could not create the save folder: " + ec.message() };
    }

    // Keep whatever is being replaced. Its own suffix, so this neither disturbs
    // the `.bak` the save rotation owns nor the `.manual.bak` rollback point.
    if (std::filesystem::is_regular_file(destination, ec)) {
        std::filesystem::path backup = destination;
        backup += ".pre-import.bak";
        const std::string failure = staged_copy(destination, backup);
        if (!failure.empty()) {
            return { false, "Could not back up the save that is already there, so nothing was "
                            "changed: " + failure };
        }
    }

    const std::string failure = staged_copy(source, destination);
    if (!failure.empty()) {
        return { false, "Could not copy the save file into place: " + failure };
    }

    return { true, "Imported. Start the game and load your file as usual. The save that was here "
                   "has been kept as " + destination.filename().string() + ".pre-import.bak" };
}

void on_saves_import() {
    if (!saves_ctx.importable) {
        return;
    }

    // The warning comes BEFORE the picker, not after it. Partly so the choice to
    // overwrite is made knowingly rather than as an afterthought, and partly
    // because opening a prompt from a file-dialog callback means opening it from
    // whatever thread delivered that callback -- which is a road this UI has
    // already been down once, with the prompt context wedging for the session.
    recompui::open_choice_prompt(
        "Import a save file?",
        "This replaces the save this app is using. The current one is kept alongside it as a "
        ".pre-import.bak file, and can be put back by hand.",
        "Choose a file",
        "Cancel",
        []() {
            set_message("Choose a save file (.bin).");
            goemon64::open_file_dialog([](bool success, const std::filesystem::path& path) {
                if (!success) {
                    // Cancelled. Leaving the "choose a file" line up would imply
                    // the picker is still waiting for them.
                    set_message({});
                    return;
                }
                const ImportOutcome outcome = import_save(path);
                refresh_state();
                set_message(outcome.message);
            });
        },
        []() {},
        recompui::ButtonVariant::Success,
        recompui::ButtonVariant::Tertiary,
        /*focus_on_cancel=*/true,
        "saves_import_button"
    );
}

} // namespace

void recompui::make_saves_bindings(Rml::Context* context) {
    Rml::DataModelConstructor constructor = context->CreateDataModel("saves_model");
    if (!constructor) {
        throw std::runtime_error("Failed to make RmlUi data model for the saves menu");
    }

    constructor.Bind("saves_path", &saves_ctx.save_path);
    constructor.Bind("saves_detail", &saves_ctx.save_detail);
    constructor.Bind("saves_message", &saves_ctx.message);
    constructor.Bind("saves_importable", &saves_ctx.importable);
    constructor.Bind("saves_has_save", &saves_ctx.has_save);

    // Same contract as every other config tab's description panel: the focused
    // row's index picks the paragraph.
    constructor.Bind("cur_config_index", &saves_ctx.focused_option);
    constructor.BindEventCallback("set_cur_config_index",
        [](Rml::DataModelHandle model_handle, Rml::Event& event, const Rml::VariantList& inputs) {
            int option_index = inputs.at(0).Get<size_t>();
            // mouseout bubbles, so only the element owning the row clears it.
            if (option_index == -1 && event.GetType() == "mouseout"
                    && event.GetCurrentElement() != event.GetTargetElement()) {
                return;
            }
            saves_ctx.focused_option = option_index;
            model_handle.DirtyVariable("cur_config_index");
        });

    saves_ctx.model_handle = constructor.GetModelHandle();

    refresh_state();
}

void recompui::register_saves_events(recompui::UiEventListenerInstancer& listener) {
    recompui::register_event(listener, "saves_import",
        [](const std::string& /*param*/, Rml::Event& /*event*/) {
            on_saves_import();
        });
}

void recompui::tick_saves() {
    // Only the game-started answer is checked per frame; it is an atomic load.
    // Everything else costs file I/O and is refreshed only when that flips,
    // which happens at most once per run.
    const bool started = ultramodern::is_game_started();
    if (started == saves_ctx.game_started) {
        return;
    }
    refresh_state();
}
