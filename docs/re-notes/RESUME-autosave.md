# RESUME — Autosave feature

Handoff note for a fresh session. Written 2026-07-18, at the point where the
implementation is complete and builds, but nothing has run on device.

## Read these first

- `docs/autosave.md` — the full feature rundown: what it does, how it works, the
  slot guard, files touched, and the complete verification procedure
- `docs/re-notes/goemon_save_re.md` — the RE behind it: save path, safe-state
  gate, residency map, per-claim confidence levels, open questions

Those two are the source of truth. This file is just the "where were we".

## State

Implemented, builds, **verified on device 2026-07-18**, and **committed and
pushed to `origin/dev`** with CI green. Commits: `70d3e4d` feature,
`9522fe8` docs + RE corrections, `49fedf8` `.bak` hazard reframing,
`71e56a5` evidence corpus, `40822a8` CLAUDE.md pointers.

What the on-device session established:

- The manual trigger commits a real save that the game loads normally
  (`status 0`), to the correct slot — confirmed on a **non-zero** slot, so the
  cursor mechanism is real rather than a BSS-zero coincidence.
- **The differential test passed.** The `0x304` payload is byte-identical to the
  game's own save routine's output apart from the CRC word and a play-time byte.
- `SAVE_SLOT_COUNT = 3` **confirmed** — the last MEDIUM-confidence assumption.
- The safety gate correctly refuses mid-dialogue (observed on the script-VM
  guard) and during every other excluded state.
- A header divergence escalated mid-session **resolved as a non-defect**: the
  `0xF0`-byte delta is uninitialised stack residue leaked by the *game's own*
  step 10, which the reimplementation reproduces faithfully. Nothing was missing
  and nothing needed fixing. It does mean a whole-file `cmp` is a false failure —
  see `docs/autosave.md`.

One bug was found and fixed in the process: the gate's phase guard was
**inverted** (`0x800C7A9E`: gameplay is 1, not 0), which refused every save. A
second, briefly self-inflicted, was adding the gate's `CONT_START` bit as a
safety guard — it is a trigger, not a guard. Both are documented in
`goemon_save_re.md`.

Autosave commits progress via Goemon's own save data and slot format by
reimplementing the game's save routine (`func_80214D58_5D0228`, which is module
code in `.file_12` and so not callable from gameplay). Every function it calls is
in `.main` and always resident, so the sequence is reproduced in
`goemon_save_now()`.

Currently **manual trigger only** — `L + R + D-Pad Up`, edge-triggered. This is
step 1 of a staged rollout; the timer is deliberately not wired up until the
write path is proven against a real save file.

The setting is in the General config menu and defaults to **Off**, because this
overwrites the player's real save.

## Next steps, in order

1. ~~**Commit.**~~ Done — see State above.
2. ~~**Sharpen the differential test.**~~ **Done, PASSED** (2026-07-18 23:12).
   Run under genuinely changed state: current HP `11 -> 10`, ryo `533 -> 363`,
   plus a counter at `+0x7C` and play time. Autosave and in-game NPC save
   differed in the slot region by exactly the mask and nothing else, with both
   writers agreeing on all four fields. Fixtures `10`/`11`; details in
   `docs/autosave.md` § "Sharpened result".

   Two field mappings fell out and are now in `goemon_save_re.md`: `+0x70` is
   **current HP** (HIGH), and `+0x7C` is an unidentified counter (MEDIUM). Note
   `+0x6C` is *max* HP — taking damage does not move it, which is a trap when
   picking a field to exercise.

   **Marshaling correctness is now closed.** Add any future pairs to
   `docs/re-notes/fixtures/` rather than overwriting — that directory is the
   evidence corpus for every claim in these notes, not backups.
3. **NEXT — build the rollback mechanism.** This *gates* the timer: it does not
   ship until a rollback exists that does not depend on `.bak`.
   **DECIDED 2026-07-18** —
   neither of the two originally-framed options, but a third: **notify +
   `.manual.bak`**. `RECOMP_PATCH func_8000B718_C318` to tell the host a
   deliberate save is happening; the host arms a one-shot and copies the file to
   `.manual.bak` on the next flush. Full design, rationale and the rejected
   alternatives are in `docs/autosave.md` § "Decided design".

   Three things to carry into the implementation:
   - The autosave path must **actively disarm** an armed one-shot, not merely
     skip arming — 12 of the 25 `func_8000B718_C318` call sites are the RAM-only
     suspend, which arms without producing a flush.
   - Two preconditions. First, the fixtures `08`/`09`/`11` byte-identity check —
     the notify patch touches the manual save path for the first time. Second,
     establish whether the two **pak diagnostic** writers are runtime-reachable.
     Note the real stakes: they write an incrementing byte pattern into slot 0,
     so if reachable they **destroy the live save directly**, autosave or not —
     poisoning `.manual.bak` is only the secondary casualty. The empirical bound
     says they probably are not reachable (the fixtures corpus spans many boots
     with slot data intact), so expect to be confirming dead or menu-gated code.
   - The dedicated slot is **parked**; its four open questions no longer gate.

4. **Then the save-data-settled check.** Zelda64Recomp's approach: diff a
   whitelist of stable save fields, require ~10 frames unchanged before writing,
   so a write never lands mid-transaction (an item being consumed, a flag being
   applied). This protects the *autosave itself*; step 3 protects everything
   else. They are independent, and **both** are prerequisites for defaulting the
   timer On.

5. **Then the timer.** Confirmed to be the right trigger shape rather than a
   default chosen for lack of options: Goemon has **no automatic commit points**
   to hook — all 12 pak-write sites are player-confirmed — so no alternative
   design is being passed over. Keep it Off by default until 3 and 4 are done.

   **The `.bak` hazard is already true of step 1, today.** `files.cpp:39-45` rotates
   `current -> .bak` on every flush and every autosave is a flush, so a *second*
   combo press makes `.bak` an autosave-of-an-autosave — observed at 2.7s apart,
   with no timer involved. Anyone testing the current build should treat the
   `adb` backup as the only trustworthy recovery copy. Full reasoning, including
   why `.bak`'s designed torn-write protection survives while its incidental
   "holds your last manual save" property does not, is in `docs/autosave.md`.

## Deferred / parked

- On-screen "Saved" indicator — no transient toast exists in this project;
  would need building from the RT64 extended-GBI path
- Save import/export via SAF — user asked, then parked. `LauncherActivity`
  already has the pattern (`ActivityResultContracts.OpenDocument` +
  a streaming `copyRom`). Must be launcher-only, or go through
  `ultramodern::change_save_file`, since the saving thread clobbers an import
  made while the game runs
- Backup-before-first-overwrite

## Environment notes

- Build is one call: `bash ~/goemon-build-all.sh` (patches -> N64Recomp ->
  file_to_c -> gradle). Note its `WIN_APK` copy path points at a stale scratchpad
  directory; the APK itself lands at
  `android/app/build/outputs/apk/debug/app-debug.apk`
- Native Linux `adb` sees no device under WSL2. Use `adb.exe` (WSL interop, on
  the PATH) or `usbipd attach`. Test device is a Retroid Pocket 5
- After any `assets/` change, delete the device's `files/data/.assets_version`
  stamp or the app keeps the OLD extracted UI

## Corrections this work produced

The 2026-07-18 session added a batch, all now written into
`docs/autosave.md` and `docs/re-notes/goemon_save_re.md`:

- `0x800C7A9E` is a 3-phase counter with gameplay == **1**, not a state index
  with gameplay == 0. The top-level state is a separate byte, `0x800C7A94`
  (gameplay `0x0D`, confirmed on device). `0x800C7A9F` is not "mode changed".
- The gate's `0x1000` bit is `CONT_START` — a trigger, not a safety guard.
- The save routine is in `.file_12`, not `.file_23`, and is dispatched by the
  **GEV script VM**, not a menu state machine.
- `func_8000B718_C318` has 24 script-level callers, half of them a RAM-only
  suspend — which is what the *"power off will Erase it"* message means.
- The slot cursor is `0x800C7D00`, is **0 at boot** (not garbage), and `-1` is a
  real sentinel; the unsigned bounds check is load-bearing.
- Step 10 writes `0x100` bytes and leaks `0xF0` bytes of stack residue, so a
  whole-file `cmp` in a differential test is a **false failure**.
- Goemon has **no automatic save points**; all 12 pak-write sites are
  player-confirmed.

The two below are older; both are documented in `goemon_save_re.md`, and
neither has been fixed at the source:

- `patches/cheats.c` says `func_8000B718_C318` commits the live block "on area
  transitions". It does not — the save path (the `jal` from the save routine,
  plus the GEV save and suspend scripts) is its only caller class, so it is a
  save-time commit, never an area-transition one. **Do not restate this as "its
  only caller in the entire binary"** — that phrasing is true only of the `jal`
  and is refuted 13 lines above; it has now misled two readers
- `*0x8020CA2C`, used as an "in gameplay" gate by `patches/cheats.c` and
  `patches/camera.c`, is `&System->controllers[i]` and is never cleared. It stays
  "valid" through pause menus, dialogue and cutscenes. Fine for those two
  features, not adequate for gating a save — which is why `patches/autosave.c`
  re-evaluates the game's own pause-gate predicate instead
