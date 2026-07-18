#ifndef PATCH_CHEATS_H
#define PATCH_CHEATS_H

// Per-frame cheat enforcement. Call once per frame from a patched per-frame
// function; re-applies whichever resource locks are enabled in the Cheats menu.
// Self-gating — does nothing outside gameplay.
void update_cheats(void);

#endif // PATCH_CHEATS_H
