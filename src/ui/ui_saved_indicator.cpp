// A transient "Saved" toast shown over live gameplay when the autosave feature
// commits a save (timed or via the manual L + R + Z combo).
//
// Why this is its own context rather than recompui::open_notification: that
// function is the modal prompt with its buttons hidden (ui_prompt.cpp:405). It
// dims the screen, closes whatever menu the player has open, captures input,
// and has no auto-dismiss -- every one of which is wrong here. The only thing
// this file needs that the prompt gives is a document to put a Label in, so it
// builds its own from create_context() and opts out of input entirely.
//
// THREADING: show_saved_indicator() is called from the guest thread (via
// recomp_notify_saved). RmlUi is not thread-safe and the document is owned by
// the render thread, so the guest side only ever stores to an atomic. Every
// document mutation happens in tick_saved_indicator(), which runs on the render
// thread from draw_hook.
//
// ORDERING: tick_saved_indicator() calls the public show_context/hide_context,
// which take ui_state_mutex internally. That mutex is a std::recursive_mutex
// (ui_state.cpp:433) and draw_hook itself re-enters it while holding it (e.g. the
// show_context at ui_state.cpp:738), so re-locking on the render thread does NOT
// deadlock -- that is NOT why the tick runs early. The real reason: the tick must
// run before draw_hook's launcher-return check (ui_state.cpp:569) so an expiring
// toast updates is_any_context_shown() before that check reads it; otherwise a
// dismissing toast suppresses the launcher for a frame. It sits alongside the
// launcher's own show_context (ui_state.cpp:562) for that same ordering reason.

#include <atomic>
#include <cstdio>
#include <chrono>

#include "recomp_ui.h"

#include "elements/ui_element.h"
#include "elements/ui_label.h"

namespace {
    // steady_clock, not system_clock: this measures an elapsed expiry interval,
    // and a wall-clock jump (NTP correction, manual time change) must not make the
    // toast linger or vanish early.
    using clock = std::chrono::steady_clock;

    // How long the toast stays up. Long enough to notice without lingering
    // over gameplay; well under the 2 minute autosave interval either way.
    constexpr clock::duration saved_indicator_duration = std::chrono::milliseconds{ 2000 };

    struct {
        recompui::ContextId ui_context = recompui::ContextId::null();
        recompui::Label* label = nullptr;

        // Set by the guest thread, consumed by the render thread. Only ever
        // goes false->true off-thread, so a plain flag is sufficient; no
        // timestamp is passed across, since the render thread starting the
        // clock when it first sees the save is accurate to within a frame.
        std::atomic<bool> pending{ false };

        // Render thread only.
        bool shown = false;
        clock::time_point hide_at = {};
    } indicator_state;
}

void recompui::init_saved_indicator_context() {
    ContextId context = create_context();

    indicator_state.ui_context = context;

    context.open();

    // The one pair of calls that separates a toast from a modal: without these
    // the document lands in is_context_capturing_input(), which makes
    // recomp::game_input_disabled() true (input.cpp:855) and freezes the game
    // the instant a save lands.
    context.set_captures_input(false);
    context.set_captures_mouse(false);

    Element* wrapper = context.create_element<Element>(context.get_root_element());
    wrapper->set_display(Display::Flex);
    wrapper->set_position(Position::Absolute);
    wrapper->set_top(0);
    wrapper->set_right(0);
    wrapper->set_bottom(0);
    wrapper->set_left(0);
    wrapper->set_align_items(AlignItems::FlexEnd);
    wrapper->set_justify_content(JustifyContent::FlexEnd);
    wrapper->set_background_color({ 0, 0, 0, 0 });

    Element* card = context.create_element<Element>(wrapper);
    card->set_display(Display::Flex);
    card->set_margin(32, Unit::Dp);
    card->set_padding_top(10, Unit::Dp);
    card->set_padding_bottom(10, Unit::Dp);
    card->set_padding_left(20, Unit::Dp);
    card->set_padding_right(20, Unit::Dp);
    card->set_border_width(1.1, Unit::Dp);
    card->set_border_radius(12, Unit::Dp);
    card->set_border_color(Color{ 255, 255, 255, 51 });
    card->set_background_color(Color{ 8, 7, 13, 217 });

    indicator_state.label = context.create_element<Label>(card, "Saved", LabelStyle::Small);

    context.close();
}

void recompui::show_saved_indicator() {
    // Guest thread. Do not touch the document here -- see THREADING above.
    indicator_state.pending.store(true, std::memory_order_relaxed);
}

void recompui::tick_saved_indicator() {
    // Render thread, from draw_hook, before ui_state_mutex is taken.
    if (indicator_state.ui_context == ContextId::null()) {
        return;
    }

    // exchange, not load+store: a save landing in the same frame as an expiry
    // must not be dropped.
    bool triggered = indicator_state.pending.exchange(false, std::memory_order_relaxed);

    if (triggered) {
        indicator_state.hide_at = clock::now() + saved_indicator_duration;

        // show_context asserts if the context is already shown
        // (ui_state.cpp:342), so a re-trigger while visible must only extend
        // the deadline set above.
        if (!indicator_state.shown) {
            show_context(indicator_state.ui_context, "");
            indicator_state.shown = true;
            // Proves the render thread received the guest's signal AND acted on
            // it. Without this, a toast that never appears is ambiguous between
            // "the save never signalled" and "it signalled but nothing drew" --
            // and this feature has already been bitten three times by a silent
            // path that looked exactly like a working one.
            printf("[autosave] indicator shown\n");
        }
        else {
            printf("[autosave] indicator re-triggered while visible\n");
        }
    }
    else if (indicator_state.shown && clock::now() >= indicator_state.hide_at) {
        hide_context(indicator_state.ui_context);
        indicator_state.shown = false;
        printf("[autosave] indicator hidden\n");
    }
}
