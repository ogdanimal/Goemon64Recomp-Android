#ifndef PATCH_AUTOSAVE_H
#define PATCH_AUTOSAVE_H

#include "patches.h"

// Commit the player's progress to the Controller Pak, using the same sequence
// as the game's own save routine. Returns the game's save status code
// (0 == success). Safe to call only from ordinary gameplay -- see
// autosave_is_safe() in autosave.c.
s32 goemon_save_now(void);

// Per-frame autosave poll. Call once per frame from a patched per-frame
// function. Self-gating -- does nothing when the setting is off or when the
// game is not in a safe state.
void update_autosave(void);

#endif // PATCH_AUTOSAVE_H
