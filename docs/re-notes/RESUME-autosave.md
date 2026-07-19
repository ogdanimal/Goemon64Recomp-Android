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

Implemented, builds, and **verified on device 2026-07-18**. Still **nothing is
committed** — all changes are in the working tree on branch `dev`.

What the on-device session established:

- The manual trigger commits a real save that the game loads normally
  (`status 0`), to the correct slot — confirmed on a **non-zero** slot, so the
  cursor mechanism is real rather than a BSS-zero coincidence.
- **The differential test passed.** The `0x304` payload is byte-identical to the
  game's own save routine's output apart from the CRC word and a play-time byte.
- `SAVE_SLOT_COUNT = 3` **confirmed** — the last MEDIUM-confidence assumption.
- The safety gate correctly refuses mid-dialogue (observed on the script-VM
  guard) and during every other excluded state.

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

1. **Commit.** Nothing is committed yet; the working tree holds the whole
   feature plus the doc corrections.
2. **Sharpen the differential test** (small, worthwhile). The passing test
   compared two saves of essentially identical state — an "equal when nothing
   changed" result. Change something the payload must record, then autosave and
   NPC-save and confirm these moved *identically* in both: `+0x6C` hearts,
   `+0x74` ryo, `+0x200` stage id. Compare with `cmp -i 256` — a whole-file
   `cmp` gives a **false failure** (see `docs/autosave.md`).
3. **Then step 2 of the rollout**: the timer, plus Zelda64Recomp's
   save-data-settled check (whitelist diff, N frames unchanged) so a write never
   lands mid-transaction. A timer is now *confirmed* to be the right shape —
   Goemon has no automatic commit points to hook, so there is no alternative
   design being passed over.
4. **Gating precondition on the timer:** it does not ship until a rollback
   mechanism exists that does not depend on `.bak` — either a dedicated slot or
   backup-before-first-overwrite. One of those two is a prerequisite now, not
   deferred polish.

   **This is already true of step 1, today.** `files.cpp:39-45` rotates
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
  transitions". It does not — the save routine is its only caller in the entire
  binary, so it is a save-time commit
- `*0x8020CA2C`, used as an "in gameplay" gate by `patches/cheats.c` and
  `patches/camera.c`, is `&System->controllers[i]` and is never cleared. It stays
  "valid" through pause menus, dialogue and cutscenes. Fine for those two
  features, not adequate for gating a save — which is why `patches/autosave.c`
  re-evaluates the game's own pause-gate predicate instead
