#ifndef __GOEMON_SAVE_ROLLBACK_H__
#define __GOEMON_SAVE_ROLLBACK_H__

namespace goemon64 {

// Installs the librecomp save observation hooks that maintain the `.manual.bak`
// rollback point. Call once during startup, before ultramodern::init_saving().
void init_save_rollback();

// Bracket for the autosave's own pak traffic. goemon_save_now() sets this for
// the whole of its body -- see patches/autosave.c -- so the observer can tell
// autosave writes from deliberate, player-initiated ones.
void set_autosave_in_progress(bool in_progress);

} // namespace goemon64

#endif
