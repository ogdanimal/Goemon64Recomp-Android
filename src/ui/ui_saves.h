#ifndef __UI_SAVES_H__
#define __UI_SAVES_H__

#include "RmlUi/Core.h"

namespace recompui {
    class UiEventListenerInstancer;

    // The "Saves" settings category: importing a save file the player already
    // has -- from another device, another port, or a backup -- into the place
    // this app reads its save from.
    //
    // It exists because that is otherwise close to impossible on Android.
    // Scoped storage puts the save under Android/data, which a file manager
    // will not open and which needs adb to reach, and doing it by hand is easy
    // to get wrong in ways that leave the app unable to save at all. Picking
    // the file through the system document picker sidesteps every part of that:
    // the app copies the bytes itself, so the result is a file it owns, in the
    // right place, with the right name.
    void make_saves_bindings(Rml::Context* context);
    void register_saves_events(UiEventListenerInstancer& listener);

    // Keeps the tab's "can I import right now" state in step with whether the
    // game has started. Render thread, once per frame; does no file I/O unless
    // that answer actually changes.
    void tick_saves();
}

#endif
