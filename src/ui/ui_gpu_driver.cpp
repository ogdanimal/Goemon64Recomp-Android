#include <stdexcept>
#include <string>
#include <vector>

#include "ui_gpu_driver.h"

#include "recomp_ui.h"
#include "goemon_support.h"

// The "GPU Driver" settings category.
//
// Android devices with a broken vendor Vulkan driver -- the Adreno 630 class of
// crash this feature exists for -- can run the game on a driver the user
// supplies instead. Everything here is a front end for GpuDriverStore.java,
// which owns the drivers on disk, the selection, and the recovery state; this
// file only asks it questions and passes answers back.
//
// THE RECOVERY MODEL, because it is what shapes this file. The menu below is
// drawn by the very renderer whose driver is in question, so it cannot be the
// only way out: a driver that fails to start would make its own undo
// unreachable. Two mechanisms cover that between them.
//
//  - A driver that takes the process down is caught by the crash latch, which
//    reverts it on the next launch and reports it from the launcher, outside
//    the game.
//  - A driver that starts but never puts anything on screen -- a black screen,
//    a hang -- is caught by the confirmation prompt below: it is armed the same
//    way, and only the user answering it disarms it. Not seeing the prompt is
//    therefore the revert signal, which is exactly the case a process-death
//    latch cannot see.
//
// That is why the prompt is not a courtesy and must not be made skippable or
// auto-dismissed. Once a driver has been confirmed it is not asked about again;
// keeping the renderer alive for a while is then enough (see the tick below).

namespace {

struct GpuDriverModelContext {
    // Row 0 is always the system driver, so `ids` runs parallel to `names` with
    // an empty id in front. Keeping both means the UI never has to convert an
    // index into an id by guessing at the ordering.
    std::vector<std::string> names;
    std::vector<std::string> ids;
    int index = 0;

    std::string detail;
    std::string selected_name;
    std::string device;
    std::string loader;
    std::string message;
    bool supported = false;
    bool removable = false;

    // Which option the user is on, for the description panel. Each config tab
    // keeps its own; see bind_config_list_events in ui_config.cpp.
    int focused_option = -1;

    // The selection as the store last reported it. Changing the list contents
    // makes RmlUi fire the select's change event, so "did the user pick
    // something" has to be answered by comparing ids rather than by trusting
    // that an event means an intent.
    std::string active_id;

    // The id the confirmation prompt on screen is about. Held so an answer given
    // slowly cannot be applied to a driver selected in the meantime.
    std::string pending_confirm_id;

    // A restart prompt the user's selection asked for, to be opened on the NEXT
    // frame rather than from the event that requested it. Opening a prompt from
    // inside the driver select's change event leaves the prompt drawn behind the
    // settings menu and deaf to input -- and it stays that way for the rest of
    // the session, breaking the app's own quit and restart prompts too. Measured
    // on device, and the discriminating control was using the select WITHOUT
    // opening a prompt, after which prompts still worked. Same shape as the
    // deferral binding-scan cancellation needs: get off the event path first.
    bool restart_prompt_pending = false;

    Rml::DataModelHandle model_handle;
};

GpuDriverModelContext driver_ctx;

#if defined(__ANDROID__)

// How long to wait before asking the user to confirm an unconfirmed driver.
// Frames rather than seconds because the question is whether the renderer is
// producing any, and a couple of them is enough for the menu to have been drawn.
constexpr uint64_t kConfirmPromptDelayFrames = 120;

// How many frames a CONFIRMED driver has to keep rendering before this launch
// counts as good and the crash latch is disarmed. Long enough to be past the
// Adreno fault, which lands about a second after the first frame, and short
// enough that an ordinary session banks it. A session shorter than this is
// covered separately by MainActivity.onDestroy.
constexpr uint64_t kSurvivedFrames = 900;

void refresh_from_store();

void set_message(const std::string& message) {
    driver_ctx.message = message;
    if (driver_ctx.model_handle) {
        driver_ctx.model_handle.DirtyVariable("driver_message");
    }
}

void offer_restart(const std::string& header, const std::string& body) {
    recompui::open_choice_prompt(
        header,
        body,
        "Restart now",
        "Later",
        []() {
            goemon64::request_restart(goemon64::RestartTarget::AppMenu);
        },
        []() {},
        recompui::ButtonVariant::Warning,
        recompui::ButtonVariant::Tertiary,
        /*focus_on_cancel=*/true,
        "driver_select"
    );
}

void on_driver_selected(int index) {
    if (index < 0 || index >= static_cast<int>(driver_ctx.ids.size())) {
        return;
    }
    const std::string id = driver_ctx.ids[index];
    // Refreshing the list re-fires this; only a different driver is a decision.
    if (id == driver_ctx.active_id) {
        return;
    }

    goemon64::android_gpu_driver_select(id);
    refresh_from_store();

    // Ask about restarting on the next frame, not here -- see restart_prompt_pending.
    driver_ctx.restart_prompt_pending = true;
}

// The selection is already saved by the time this runs; all that is left to decide
// is when it takes effect. It cannot be now: the driver is handed to Vulkan while
// the process starts, so a change needs a new one.
void drain_restart_prompt() {
    if (!driver_ctx.restart_prompt_pending) {
        return;
    }
    driver_ctx.restart_prompt_pending = false;
    offer_restart(
        "Restart to use it?",
        driver_ctx.selected_name + " will be used the next time the game starts. "
        "Restarting now loses any progress since your last save."
    );
}

void on_driver_remove() {
    if (driver_ctx.index <= 0 || driver_ctx.index >= static_cast<int>(driver_ctx.ids.size())) {
        return;
    }
    const std::string id = driver_ctx.ids[driver_ctx.index];
    const std::string name = driver_ctx.names[driver_ctx.index];

    recompui::open_choice_prompt(
        "Remove this driver?",
        name + " will be deleted from this app. You can import it again later.",
        "Remove",
        "Cancel",
        [id]() {
            goemon64::android_gpu_driver_remove(id);
            refresh_from_store();
        },
        []() {},
        recompui::ButtonVariant::Error,
        recompui::ButtonVariant::Tertiary,
        /*focus_on_cancel=*/true,
        "driver_remove_button"
    );
}

void on_driver_import() {
    set_message("Choose a driver package (.adpkg) or a driver library (.so).");
    goemon64::android_gpu_driver_request_import([](bool success, const std::string& message) {
        if (success) {
            refresh_from_store();
            // Importing is not selecting: switching the renderer under someone who
            // was only adding a file would make a crash-latch cycle their
            // introduction to the feature.
            set_message("Imported " + message + ". Choose it above to use it.");
        }
        else if (!message.empty()) {
            set_message(message);
        }
        else {
            // Cancelled. There is nothing to report, and leaving the "choose a
            // file" line up would suggest the picker is still waiting.
            set_message({});
        }
    });
}

void refresh_from_store() {
    goemon64::GpuDriverState state = goemon64::android_gpu_driver_state();

    driver_ctx.names.clear();
    driver_ctx.ids.clear();
    driver_ctx.names.emplace_back("System driver");
    driver_ctx.ids.emplace_back("");

    int index = 0;
    for (const goemon64::GpuDriverEntry& entry : state.drivers) {
        driver_ctx.names.push_back(entry.name);
        driver_ctx.ids.push_back(entry.id);
        if (!entry.id.empty() && entry.id == state.active_id) {
            index = static_cast<int>(driver_ctx.ids.size()) - 1;
            driver_ctx.detail = entry.detail;
        }
    }
    if (index == 0) {
        driver_ctx.detail = "The driver that came with your device.";
    }

    driver_ctx.index = index;
    driver_ctx.active_id = state.active_id;
    driver_ctx.removable = (index > 0);
    driver_ctx.selected_name = driver_ctx.names[index];
    driver_ctx.device = state.device.empty() ? std::string{ "not recorded yet" } : state.device;
    driver_ctx.loader = state.loader;

    if (driver_ctx.model_handle) {
        driver_ctx.model_handle.DirtyAllVariables();
    }
}

#endif // __ANDROID__

} // namespace

void recompui::make_gpu_driver_bindings(Rml::Context* context) {
    Rml::DataModelConstructor constructor = context->CreateDataModel("driver_model");
    if (!constructor) {
        throw std::runtime_error("Failed to make RmlUi data model for the GPU driver menu");
    }

    // No RegisterArray for the driver name list. RmlUi's type register belongs to
    // the Context, not to one data model, so a type may be registered exactly
    // once across all of them -- a second attempt logs an error and is ignored.
    // make_debug_bindings already registers std::vector<std::string> and runs
    // first (see ConfigMenu::make_bindings), which is what makes the Bind below
    // legal. If that registration ever moves, this Bind fails at startup with an
    // RmlUi error naming the type, and the fix is to register it once somewhere
    // that still precedes every user.
    constructor.Bind("driver_supported", &driver_ctx.supported);
    constructor.Bind("driver_names", &driver_ctx.names);
    constructor.Bind("driver_index", &driver_ctx.index);
    constructor.Bind("driver_detail", &driver_ctx.detail);
    constructor.Bind("driver_selected_name", &driver_ctx.selected_name);
    constructor.Bind("driver_device", &driver_ctx.device);
    constructor.Bind("driver_loader", &driver_ctx.loader);
    constructor.Bind("driver_message", &driver_ctx.message);
    constructor.Bind("driver_removable", &driver_ctx.removable);

    // Same contract as every other config tab's option-description panel: the
    // focused row's index drives which paragraph is shown. Bound per model, so
    // this is the driver tab's own copy rather than shared state.
    constructor.Bind("cur_config_index", &driver_ctx.focused_option);
    constructor.BindEventCallback("set_cur_config_index",
        [](Rml::DataModelHandle model_handle, Rml::Event& event, const Rml::VariantList& inputs) {
            int option_index = inputs.at(0).Get<size_t>();
            // mouseout bubbles, so only the element that owns the row clears it.
            if (option_index == -1 && event.GetType() == "mouseout"
                    && event.GetCurrentElement() != event.GetTargetElement()) {
                return;
            }
            driver_ctx.focused_option = option_index;
            model_handle.DirtyVariable("cur_config_index");
        });

    driver_ctx.model_handle = constructor.GetModelHandle();

#if defined(__ANDROID__)
    driver_ctx.supported = goemon64::android_gpu_driver_supported();
    if (driver_ctx.supported) {
        refresh_from_store();
    }
#endif
}

void recompui::register_gpu_driver_events(recompui::UiEventListenerInstancer& listener) {
#if defined(__ANDROID__)
    recompui::register_event(listener, "gpu_driver_selected",
        [](const std::string& /*param*/, Rml::Event& event) {
            on_driver_selected(event.GetParameter<int>("value", 0));
        });
    recompui::register_event(listener, "gpu_driver_import",
        [](const std::string& /*param*/, Rml::Event& /*event*/) {
            on_driver_import();
        });
    recompui::register_event(listener, "gpu_driver_remove",
        [](const std::string& /*param*/, Rml::Event& /*event*/) {
            on_driver_remove();
        });
#else
    // The tab is hidden off Android, but the document still names these handlers
    // and RmlUi warns about every one it cannot instance.
    recompui::register_event(listener, "gpu_driver_selected",
        [](const std::string& /*param*/, Rml::Event& /*event*/) {});
    recompui::register_event(listener, "gpu_driver_import",
        [](const std::string& /*param*/, Rml::Event& /*event*/) {});
    recompui::register_event(listener, "gpu_driver_remove",
        [](const std::string& /*param*/, Rml::Event& /*event*/) {});
#endif
}

void recompui::tick_gpu_driver() {
#if defined(__ANDROID__)
    if (!goemon64::android_gpu_driver_supported()) {
        return;
    }

    static uint64_t frames = 0;
    static bool asked = false;
    static bool noted_survived = false;
    // What was running when this process started. Captured on the first frame,
    // before the user can have changed anything, because the question the prompt
    // asks is about the driver drawing the screen it appears on -- not about
    // whatever happens to be selected by the time they answer.
    static bool boot_needs_confirmation = false;
    static std::string boot_driver_id;
    static std::string boot_driver_name;

    frames++;

    // First, because a selection the user just made is the most immediate thing
    // waiting, and it must be opened from here rather than from the event that
    // asked for it.
    drain_restart_prompt();

    if (frames == 1) {
        goemon64::GpuDriverState state = goemon64::android_gpu_driver_state();
        boot_needs_confirmation = state.needs_confirmation;
        boot_driver_id = state.active_id;
        for (const goemon64::GpuDriverEntry& entry : state.drivers) {
            if (entry.id == boot_driver_id) {
                boot_driver_name = entry.name;
            }
        }
        // The menu's copy of the state was taken when the menus were built, which
        // is BEFORE the renderer reports which device it came up on -- so it says
        // "not recorded yet" for the launch the user is looking at. One frame in,
        // that report has happened.
        refresh_from_store();
        return;
    }

    // Asked once per launch. A driver nobody has vouched for is reverted on the
    // next launch unless the user says otherwise here, so this prompt is the only
    // thing standing between them and a driver that renders nothing at all.
    if (!asked && frames >= kConfirmPromptDelayFrames) {
        asked = true;
        // Selecting a driver from the menu already declares the running one good
        // (it cleared the latch), so there is nothing left to confirm and asking
        // would name a driver that is not the one on screen.
        const bool still_the_boot_driver =
            goemon64::android_gpu_driver_state().active_id == boot_driver_id;
        if (boot_needs_confirmation && still_the_boot_driver && !boot_driver_id.empty()) {
            driver_ctx.pending_confirm_id = boot_driver_id;
            recompui::open_choice_prompt(
                "Keep this graphics driver?",
                "The game is being drawn by " + boot_driver_name + ". If you can read this, "
                "it is working — keep it?\n\nIf you do not answer, the game goes back to the "
                "system driver the next time it starts.",
                "Keep it",
                "Use the system driver",
                []() {
                    goemon64::android_gpu_driver_confirm(driver_ctx.pending_confirm_id);
                    refresh_from_store();
                },
                []() {
                    goemon64::android_gpu_driver_select("");
                    refresh_from_store();
                    goemon64::request_restart(goemon64::RestartTarget::AppMenu);
                },
                recompui::ButtonVariant::Success,
                recompui::ButtonVariant::Warning,
                /*focus_on_cancel=*/false,
                ""
            );
        }
        return;
    }

    // A confirmed driver: a human has already testified that it draws the game,
    // so what is left worth catching is the process dying or the GPU hanging, and
    // both of those stop frames arriving here.
    if (!noted_survived && frames >= kSurvivedFrames) {
        noted_survived = true;
        goemon64::android_gpu_driver_note_survived();
    }
#endif
}
