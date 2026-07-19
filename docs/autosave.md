# Autosave — implementation rundown

Status as of 2026-07-18. Ported from Zelda64Recomp's `patches/autosaving.c`.

**Verified on device (2026-07-18).** The manual trigger commits a real save that
the game loads normally, and the differential test against the game's own save
routine **passed**: the `0x304` payload is byte-identical apart from the CRC word
and a play-time counter. Details in "Verifying it" below. Still manual-only —
the timer is step 2 and is not wired up.

## What it does

Commits your progress through Goemon's own save data and save-slot format, so a
resulting save loads through the normal load path and starts you where a save
made at that point would. It writes to whichever slot you loaded. No custom
format, no separate slot, no bespoke checksum.

**Current state: manual trigger only.** Step 1 of a staged rollout — the timer
is not wired up until the write path is proven on-device.

**Trigger: `L` + `R` + `D-Pad Up`**, edge-triggered (holding it saves once, not
every frame), and only while the safety gate passes. See the
`AUTOSAVE_TEST_COMBO` define in `patches/autosave.c`.

The setting defaults to **Off**. Zelda64Recomp defaults theirs On; this one
overwrites your real save, so we do not.

## How it works

### 1. The save itself

Goemon's real save routine (`func_80214D58_5D0228`) is module code in
`.file_12`, so it cannot be called from gameplay. Every function it calls lives
in `.main` and is always resident, so its sequence is **reimplemented** in
`goemon_save_now()`:

1. probe the pak (`func_800232EC_23EEC`, `func_80023410_24010`, `func_80023480_24080`)
2. validate the slot cursor (see "Slot guard" below)
3. allocate a `0xA00` scratch buffer
4. marshal live state into gamedata (`func_8000B718_C318`)
5. copy `0x304` bytes of gamedata to `buf+4`
6. CRC-32 into the header word at `buf+0` (`func_80023A1C_2461C`)
7. write the `0x500` slot (`func_80023610_24210`)
8. read it back (`func_800234D8_240D8`)
9. compare `0xC2` words — `0x308` bytes, i.e. the 4-byte CRC word plus the
   `0x304` payload
10. write the header (`func_80023698_24298`) — see the caveat below
11. free the scratch buffer

**Step 10 is not a 12-byte write.** It writes **`0x100` bytes at file offset 0**,
of which only the first `0x10` are initialised (a CRC word plus three global
progress flags from `0x8015CC18`). Bytes `0x10`–`0xFF` are **uninitialised stack
residue** that the game's own routine leaks into the file. We reproduce that
faithfully, and nothing reads past `0x0F`.

This matters only because it breaks naive file comparison — see "Success
criterion". It is not a defect and needs no fix; `memset`ing the tail would
deliberately diverge from the game for no functional gain.

### 2. The safety gate

The port of Zelda64Recomp's `gCanPause` trick. Goemon's equivalent of
`KaleidoSetup_Update` is `func_8001F940_20540` (always resident) — specifically
**entry 1** of the 3-entry phase table at `0x8006B730`. Its START test sits
second in the chain rather than last, so rather than latching a flag in a
fall-through branch, `autosave_is_safe()` re-evaluates its guard chain directly —
every guard is a fixed base-exe address.

**Two traps in that chain, both of which refused every save when got wrong:**

- `0x800C7A9E` is the **phase** counter, and running gameplay is **1**, not 0.
  Value 0 is the single init frame. An earlier note had this inverted.
- The gate's `andi 0x1000` reads a **controller button word**, and `0x1000` is
  `CONT_START`. That is the "did the player ask to pause" *trigger*, not a safety
  condition. It must **not** be a guard — holding the save combo can never
  satisfy it.

The full chain now checked, all confirmed passing on device during gameplay:
top-level state `0x800C7A94 == 0x0D`, phase `== 1`, exit-requested flag `== 0`,
not paused / locked / transferring, submode `< 4 && != 3`, the script-VM gate
`0x80077858 == 0`, `g_is_loading_file`, two further guards, and `.file_11`
resident.

That transitively excludes cutscenes, dialogue, the pause menu, area transitions
and loading. It additionally requires `.file_11` (the main 3D field/town engine)
to be the loaded stage module, which excludes the Impact battle and sidescroller
stages.

Deliberately **not** used: `*0x8020CA2C`, the gate `patches/cheats.c` and
`patches/camera.c` use. See "Corrections" below.

### 3. Persistence

Nothing new was needed. The Controller Pak shim in `librecomp/src/pak.cpp`
already routes guest writes through `save_read`/`save_write` — the identical path
a manual in-game save uses — and the debounced saving thread in
`librecomp/src/pi.cpp` flushes to disk on its own.

## The timer

**Implemented and verified on device 2026-07-19.** Saves every
`AUTOSAVE_INTERVAL_US` (2 minutes) when the gate passes, the state has settled,
and something has actually changed.

A timer is **confirmed to be the right trigger shape**, not a fallback: a
whole-ROM scan established Goemon has no automatic commit points — all 12
pak-write sites are behind explicit player confirmation — so there is nothing to
piggyback on and no better design is being passed over.

**Three behaviours, none incidental:**

1. **An elapsed interval is not consumed.** If the gate or the settled check
   refuses, the timer retries every frame instead of skipping to the next
   period. Otherwise an interval elapsing during a cutscene would silently lose
   that save.
2. **Nothing is written if nothing changed.** The settled hashes are snapshotted
   at each save and compared. This matters more than it sounds: every flush
   rotates `.bak`, so a timer that wrote unconditionally would churn the
   runtime's backup forever for no benefit. **Note walking around changes
   nothing we watch** — position is not in the payload — so idle and
   walking-only stretches produce no writes at all.
3. **A failed save still resets the timer**, so a persistently failing save
   cannot retry every frame.

The manual combo also feeds the timer, so a timed save cannot land moments after
the player saved by hand.

### Verification — and the third time silence looked like success

The first timer run produced only two lines across 75 seconds of testing. That
was **consistent with correct suppression and equally consistent with a timer
that fired once and never re-armed**, and the log could not distinguish them —
because the diagnostic only printed on success, exactly the blind spot the
settled check had.

A temporary suppression diagnostic settled it, printing the decision fields once
per suppression episode. The full cycle was then observed:

| time | event |
|---|---|
| `00:56:11.625` | `timed save -> status 0` |
| `00:56:31.625` | `suppressed: safe 1 \| settled 1 (10/10) \| changed 0` |
| `00:58:22.092` | `timed save -> status 0` — re-armed on a state change |
| `00:58:42.125` | `suppressed: ... changed 0` — back to idle |

Successive intervals measured **20.000s and 20.033s** against a temporarily
shortened 20s interval; the 33ms is two frames, i.e. the per-frame granularity.
`safe 1 | settled 1 (10/10) | changed 0` also proves the suppression had exactly
one cause and nothing else was quietly wedged.

Both the interval and the diagnostic were then restored/stripped.

**That is three separate occasions in this feature where silence looked like
success** — the differential test's header residue, the possibly-inert settled
check, and this. The pattern is consistent enough to state as a rule: *for
anything that refuses or declines, build the diagnostic that proves it is
reaching its decision point, or the passing test proves nothing.*

## The settled check

**Implemented and verified on device 2026-07-19.** Requires the save-relevant
state to hold still for `SETTLE_FRAMES` (10) before a save may commit, so a
write never lands mid-transaction. This protects the **autosave itself**;
`.manual.bak` protects everything around it. Both gate defaulting the timer On,
and both are now met.

### What is watched, and why it is two buffers

Watching gamedata alone would be **wrong**: the marshal `func_8000B718_C318` is
what copies the live player block into `gamedata+0x64`, and it runs at save
time — so between saves gamedata's HP, ryo and lives are **stale**. A check
watching only gamedata would be blind to taking damage or spending money, which
is most of what "mid-transaction" means here. Watching only the live block would
miss every event flag. So both, as four ranges:

| # | range | bytes | contents |
|---|---|---|---|
| A | `0x8015C5D8` | `0x030` | live block — max HP `+0x08`, current HP `+0x0C`, ryo `+0x10`, lives `+0x14`, counter `+0x18` |
| B | `0x8015C608` | `0x064` | gamedata `+0x000` — event-flag bitfield |
| C | `0x8015C69C` | `0x1D0` | gamedata `+0x094..+0x263` — progress, second bitfield, room/spawn |
| D | `0x8015C870` | `0x098` | gamedata `+0x268..+0x2FF` — unlock tables, gauges |

**Deliberate exclusions, each a trap rather than an omission:**

- `gd+0x064..+0x093` — the **stale mirror** of range A. Excluding it is not mere
  redundancy: it changes *only when a save runs*, so watching it would inject a
  spurious "unsettled" at exactly the moment a save is attempted.
- `gd+0x264..+0x267` — play time. Written live, `+1` every **60** frames (it is
  seconds, not frames), so it would not deadlock a 10-frame window, but it is
  noise.
- `gd+0x300..+0x303` — footer/CRC, touched at save time.
- the shadow at `0x8015C910` — written only at save/load.

Player position is **not** in the payload, so walking does not prevent settling
— confirmed on device. Room transitions **do** register as unsettled via
`+0x204..+0x211` in range C, which is wanted.

### Verification — and why a passing test nearly meant nothing

The first on-device run produced **no `not settled` refusals at all**, and every
save succeeded. That was indistinguishable from the check being **inert**: had
the addresses been wrong and read static memory, the hashes would never differ,
the counter would pin at the threshold, and every save would be permitted while
the check protected nothing. **The log would have looked identical.** This is the
same failure shape as the inverted phase guard, with the sign flipped — that one
refused everything, an inert check permits everything.

Two positive controls settled it:

1. **A temporary change-logging diagnostic** proved the hashes move and map as
   claimed: `live` on damage/ryo/health item, `progress` on room transitions,
   all four at once on a file load (the wholesale `0x300` copy). A **91-second
   stretch with no output** while playing is the evidence that no
   continuously-volatile field is in the whitelist — the chronic-refusal risk.
2. **`SETTLE_FRAMES` temporarily widened to 120** so the refusal path could be
   hit by hand. Two refusals reported `98/120` and `32/120` frames against
   wall-clock gaps of 1.634s and 0.535s — **98.0 and 32.0 frames at 60Hz**,
   matching to within a millisecond. So the counter tracks real frames rather
   than merely printing plausible output.

Both diagnostics were then stripped and `SETTLE_FRAMES` restored to 10;
recovery was confirmed under the shipping config (two saves, `status 0`).

**The lesson, again:** the absence of a refusal is not evidence of correctness.
It took a deliberately-provoked failure to distinguish a working check from an
inert one, and neither ordinary test would ever have done it.

## Slot guard

The slot cursor at `*(u32*)(G + 0x3B040)` — absolute **`0x800C7D00`** — is
written **only** by the Adventure Diary overlay. `goemon_save_now()` validates it
before allocating or marshalling, and returns `AUTOSAVE_ERR_BAD_SLOT` (`-2`) if
it is out of range.

**Corrected 2026-07-18 — the hazard is not "stale or uninitialised".** The cursor
is a field inside the `System` struct and is covered by the boot memset, so it is
**`0` at boot, guaranteed**. `0` is a *valid* slot index. So an unset cursor does
not fail the guard; it silently names **file 1**.

The Diary UI is reachable only *from* the main-engine scene, so a fresh boot
reaches that scene with the cursor never written. The `>= 3` guard structurally
cannot catch this. (MEDIUM-HIGH: the reachability is structural; whether that
window is *saveable* gameplay was not separately established, and the safety gate
adds further conditions.) If it is ever shown reachable, all three cursor writers
converge on `func_801CD890_660740` — one hook there could latch "a selection was
committed this boot" and refuse until set.

Two things this **does** confirm about the existing code:

- The new-game-on-empty-slot path **does** set the cursor (both branches fall
  through to an unconditional store), so that suspected hole is closed.
- `-1` is a real sentinel meaning "not using Controller Pak", written by two of
  the three writers. So the bounds check is load-bearing, and it must stay
  **unsigned**: `slot >= SAVE_SLOT_COUNT` on a `u32` rejects `0xFFFFFFFF`, where
  a signed compare would let it through and compute offset `-1 * 0x500 + 0x100`.
  The game's own routine has no bounds check at all.

This guard is load-bearing, not defensive tidiness. Nothing downstream
re-checks the slot:

- the pak offset is `slot * 0x500 + 0x100`
- the only bounds check in the whole chain is an `assert()` in librecomp's
  `save_write`
- that assert is **compiled out**: the `externalNativeBuild` block in
  `android/app/build.gradle` sets `-DCMAKE_BUILD_TYPE=RelWithDebInfo`, and the
  resulting `CMAKE_CXX_FLAGS_RELWITHDEBINFO` is `-O2 -g -DNDEBUG`
- the write past it is an unchecked `std::vector::operator[]`

So unguarded, the blast radius was:

| Slot value | Consequence |
|---|---|
| valid but not the player's | silently overwrites a *different* save slot — data loss, no error |
| `> 0x65` (buffer is `0x20000`) | out-of-bounds heap write in the app process |

The boundary: a `0x500`-byte write at `slot * 0x500 + 0x100` into a `0x20000`
buffer puts the last in-bounds slot at `0x65` (ending at `0x1FF00`), so `0x66` is
the first to overrun.

`SAVE_SLOT_COUNT` is **3** — originally derived from the pak file size the shim
reports in `osPfsFileState` (`0x1000`; `0x100 + 3 * 0x500 == 0x1000` exactly),
and **CONFIRMED on device 2026-07-18**: the Select Adventure Diary screen shows
exactly three entries, the third reading "From the beginning".

Note the count is visible on the **file-select** screen, not the save
confirmation — saving through an NPC offers no slot choice at all, because the
choice was made at launch.

If the inference is wrong it fails safe rather than silently: a wrongly-refused
slot returns `AUTOSAVE_ERR_BAD_SLOT` and prints the `no valid save slot selected`
message with the actual cursor value, so it is immediately diagnosable from
logcat.

## Files created

- `patches/autosave.c` — the reimplemented save routine, the safety gate, the
  per-frame poll
- `patches/autosave.h` — declares `goemon_save_now()` / `update_autosave()`
- `docs/re-notes/goemon_save_re.md` — RE writeup: save path, gate, residency map,
  confidence levels, open questions
- `docs/autosave.md` — this file

## Files modified

- `patches/main.c` — added the include and the `update_autosave()` call inside
  the per-frame `func_800012FC_1EFC` patch
- `src/main/main.cpp` — uncommented `REGISTER_FUNC(recomp_get_autosave_enabled)`
- `src/game/config.cpp` — in `set_general_settings_from_json`, default
  `AutosaveMode::On` -> `AutosaveMode::Off`
- `assets/config_menu/general.rml` — added the On/Off `<input type="radio">` pair
  bound to `autosave_mode` at config index 6, spliced into the
  `nav-up`/`nav-down` chain, and replaced Zelda64Recomp's stale description text
  (it referenced owl saves and Clock Town)
- `README.md` — Features entry

No `patches/Makefile` change needed — it globs `*.c`.

## Reused, not rebuilt

Most of the config plumbing was already in the repo as dead Zelda64Recomp
scaffolding: the `AutosaveMode` enum (`include/goemon_config.h`), JSON
persistence, getter/setter, the RmlUi binding (`src/ui/ui_config.cpp`),
`DECLARE_FUNC` (`patches/misc_funcs.h`), the `syms.ld` address, and the host impl
(`src/game/recomp_api.cpp`). Only the registration and the menu markup were
missing.

## Corrections made to existing notes

- `func_8000B718_C318` is a **save-time** commit, not an area-transition one —
  the save path (the `jal` from the save routine, plus the GEV save and suspend
  scripts) is its only caller class. The comment near the top of
  `patches/cheats.c` says otherwise and is inaccurate.
- `*0x8020CA2C` (the gate `patches/cheats.c` and `patches/camera.c` use) is
  `&System->controllers[i]` and is **never cleared**, so it stays "valid" through
  pause menus, dialogue and cutscenes. Adequate for those features; not adequate
  for gating a save.

Added 2026-07-18, from the on-device session and three static scans:

- **`func_80214D58_5D0228` lives in `.file_12`, not `.file_23`** (the comment at
  the top of `patches/autosave.c` said `.file_23`, which is a slot-A module).
- It is dispatched by the **GEV event-script VM's `0x800B` native-call opcode**,
  not by a save-menu state machine, from 12 script call sites.
- "the save routine is `func_8000B718_C318`'s only caller" is true for `jal` but
  false overall — there are 24 script-level callers, 12 of which are a
  **RAM-only suspend** (the *"turning the power off will Erase the Saved
  information"* option) rather than a pak write.
- `0x800C7A9E` semantics were inverted; `0x800C7A9F` is not "mode changed"; the
  `0x1000` bit in the gate is `CONT_START`. See "The safety gate".
- The slot cursor hazard is `0`/file-1, not staleness. See "Slot guard".
- Step 10 writes `0x100` bytes and leaks stack. See "How it works".

## Deferred

- **The timer** (step 2). The write path is now verified, so this is unblocked.
  **A timer is confirmed to be the right trigger shape**, not merely a default: a
  whole-ROM scan established that Goemon has **no automatic commit points** —
  every one of the 12 pak-write sites is behind an explicit player confirmation.
  There is nothing to hook, so the alternative of piggybacking on the game's own
  notion of a checkpoint does not exist.
- ~~**Save-data-settled check**~~ — **DONE and VERIFIED on device 2026-07-19.**
  See "The settled check" below.
- **On-screen "Saved" indicator** — no transient toast exists in this project;
  `recompui::open_notification` is a modal with no auto-dismiss, so it would need
  building from the RT64 extended-GBI path.
- ~~**Backup-before-first-overwrite**~~ — **no longer deferred.** Promoted to a
  prerequisite alongside the dedicated slot; either one satisfies the gating
  precondition in "What it overwrites, exactly". See there for why.
- **Save import/export via SAF** — `LauncherActivity` already has the pattern
  (`ActivityResultContracts.OpenDocument` plus a streaming `copyRom`). Likely a
  better mitigation than the backup-region idea below, since it also covers
  device migration. Must be launcher-only, or go through
  `ultramodern::change_save_file`, because the saving thread will clobber an
  import made while the game is running.

## What it overwrites, exactly — open questions

This section exists because every hazard in this feature so far surfaced only
when someone asked "and what does this write land on?" — the slot cursor, the
`.bak` rotation, the header residue. Asking it *before* the code, not after.

**As shipped, an autosave overwrites the file you are playing.** That is the
game's own semantic (an in-game save offers no slot choice; it overwrites the
file chosen at launch), so this is native behaviour rather than a compromise.
The consequences are still worth stating:

- **A bad autosave destroys the only copy.** Which is what the settled-check
  (below) is for.
- **`.bak` is not a recovery copy once autosave is enabled.** See below. This is
  a **step 1** property, not a timer problem.

### `.bak` no longer holds your last manual save (step 1, already true)

`lib/N64ModernRuntime/librecomp/src/files.cpp:39-45` copies `current -> .bak`
then `temp -> current` on **every flush**, and every combo press produces a
flush. So the second autosave rotates the first into `.bak` and the previous
manual save off the device entirely. Observed: two presses 2.7s apart did exactly
that.

**This is not something the timer introduces.** The demonstration used manual
presses only. Any second use of the combo makes `.bak` an autosave-of-an-
autosave; a timer merely makes that window permanent rather than
usage-dependent. Do not read step 1 as covered.

**Be precise about what was lost.** The rotation's *designed* function is
torn-write protection — if the process dies mid-copy, `.bak` still holds a
complete previous file — and that survives autosave intact: after two autosaves
`.bak` is still a complete, loadable save. What is destroyed is the *incidental*
property that `.bak` usually contained the last **manual** save, which was only
ever true because flushes were rare.

That distinction determines the fix. Restoring the old behaviour is not an
option — the host cannot distinguish an autosave flush from a manual one without
new plumbing. The goal is a **deliberate rollback story**, so nothing depends on
the accident:

- a **dedicated slot** (manual slots untouched, so `.bak` rotation becomes
  harmless and the rollback point lives inside the save file, out of reach of any
  host-side rotation), or
- **backup-before-first-overwrite** — a one-time `.pre-autosave` host-side
  snapshot, cheaper if the slot-count math makes dedicating one unattractive.

**One of these is a prerequisite, not deferred polish.** This also corrects an
earlier claim in this doc's history: the dedicated slot was pitched as *killing*
the backup-before-first-overwrite item. The opposite is true — they are two
answers to the same demonstrated loss path, and shipping neither leaves both
nets failing at once, which is precisely what was observed.

**Gating precondition:** *the timer does not ship until a rollback mechanism
exists that does not depend on `.bak`.*

### Decided design: observe the pak write + `.manual.bak`

**Decided 2026-07-18, and IMPLEMENTED.** Supersedes the notify-patch design
recorded below, which is kept because the reasoning that produced it is still
load-bearing and because a superseded copy nobody hunted down is what has
misled readers here twice already.

The rollback point is maintained by **observing guest pak writes host-side**.
No game function is patched at all.

**Mechanism:**

1. librecomp gained two **generic** hooks, neither of which knows what an
   autosave is (`ultramodern::set_save_write_callback` /
   `set_save_flush_callback`, declared in `ultramodern/ultramodern.hpp`): one
   fired on every guest mutation of the save buffer, one after a flush has been
   finalized on disk.
2. All the policy lives in `src/game/save_rollback.cpp`. A guest pak write that
   is **not** the autosave's own means a deliberate save-class operation, so it
   arms a one-shot; the next completed flush copies the save file to
   `.manual.bak`.
3. `goemon_save_now()` brackets **its entire body** with
   `recomp_set_autosave_in_progress(1/0)`, so its own traffic is excluded.

**Why this works with no patched function.** Every guest pak access already
arrives at the host fully described: the Controller Pak shim
(`librecomp/src/pak.cpp:60-77`) hands `save_write` the guest's own offset, size
and a write flag already separated from reads. And `func_80023610_24210` issues
**exactly one** `osPfsReadWriteFile` call — `size = 0x500`,
`offset = slot * 0x500 + 0x100`, `flag = 1` (`RecompiledFuncs/funcs_6.c`,
instructions `0x80023638`–`0x8002365C`) — so there is no chunking to reassemble.
Observation is not patching; that distinction is the whole of this design.

**The bracket must cover the whole of `goemon_save_now()`, not just the slot
write.** Step 10 pushes a `0x100`-byte header block through the same
`save_write` path. Bracketing only the slot write would let the autosave's own
header write arm the one-shot, and the next flush would copy autosave content
into `.manual.bak` — precisely the state it exists to roll back from. C has no
RAII, so the bracket is implemented as a thin wrapper around a
`goemon_save_now_inner()` body, which keeps every early return balanced and
makes the future timer caller inherit it rather than have to remember it.

**What this design gains over the notify patch:**

| | notify patch | pak-write observation |
|---|---|---|
| guest change | reimplement `func_8000B718_C318` | none — only our own `autosave.c` |
| fixtures `08`/`09`/`11` precondition | required | **vacuous** — manual path untouched |
| suspends arming spuriously | must actively disarm | never reach `save_write` at all |
| anchor | marshal (before the write) | the write itself |

The RAM-only suspend problem **disappears** rather than being handled: a suspend
marshals but never touches the pak, so it never reaches the observer. The
"active disarm" rule the notify design required is therefore gone, not
relaxed.

**Correctness no longer depends on the call-site enumerations.** This is worth
stating as an advantage rather than a caveat: arming on *any* non-autosave pak
write means the design does not rest on the 8-site flush map or the 25-site GEV
map being closed. An unmapped deliberate writer arms the one-shot and gets its
own game-produced state copied — which **degrades the contract gracefully
instead of violating it**. The old design's correctness rested on the closed-set
claim; this one merely benefits from it.

**Contract (unchanged):** `.manual.bak` holds *the file state as of the last
deliberate **save-class** operation* — not "the last manual save". A Diary erase
or copy is deliberate and is captured too; that is correct, since the erase was
the player's own act.

**The rollback point is maintained unconditionally**, not gated on the autosave
setting. A player who enables autosave and immediately triggers one would
otherwise have no pre-autosave rollback point, because nothing would have been
armed before the first autosave flush.

**Precondition 2 (diagnostics reachability): RESOLVED — the two `.main` pak
diagnostic writers are UNREACHABLE.** Confidence HIGH, from four independent
negative results with a working positive control. See "Diagnostics reachability"
below.

**Precondition 1 (fixtures `08`/`09`/`11` byte-identity): VACUOUS, and worth
recording as vacuous rather than deleting.** It existed only because the notify
patch would have replaced a function on the live manual-save path. This design
does not touch that path — no game function is patched — so there is no
divergence for the check to detect. If anyone ever revives the notify approach,
the precondition revives with it.

#### Diagnostics reachability — RESOLVED (2026-07-18)

The two `.main` writers are **`func_80023C14_24814`** (`RecompiledFuncs/funcs_4.c:4049`,
`jal` at `funcs_4.c:4133`) and **`func_80023CC8_248C8`** (`funcs_3.c:4631`, `jal`
at `funcs_3.c:4717`). Both confirmed destructive as described: fill `0x500`
bytes with an incrementing byte counter, then write **slot 0**.

**Verdict: UNREACHABLE, HIGH confidence.** Four independent negative results:

- **No `jal`.** Verified twice — in the recompiled C, and by encoding
  `jal 0x80023C14` / `jal 0x80023CC8` and scanning all 32 MiB: 0 hits each.
- **No fall-through.** The preceding `func_80023B60_24760` ends at `0x80023C0C`
  with `jr $ra` + delay slot, so control cannot slide in.
- **No data-word reference anywhere in the ROM** — so no GEV native-call
  dispatch and no function-pointer table entry.
- **No `lui`+`addiu` address materialization.**

**The scan had a working positive control**, which is what makes the negatives
meaningful: the same data-word scan finds `0x80214D58` (the real save routine)
**12 times** with 0 `jal` hits — exactly the GEV `0x800B` dispatch pattern, and
matching the known "12 player-confirmed pak writes". So the method demonstrably
detects the one dispatch mechanism of concern, and the GEV script data is
uncompressed and scannable.

An anomaly surfaced and was **fully explained** rather than waved through: a
range scan hit twelve copies of `0x80023B58` at rom `0x7CC14`–`0x7CC40`. That is
the jump table at `0x8007C014` for the switch in `func_80023ADC_246DC` (flagged
by N64Recomp as `switch_error` at `funcs_16.c:9236`); all twelve entries point at
a bare `jr $ra` no-op default, and **none** point at either diagnostic. A stray
`0x800238DE` at rom `0x13EA3AC` is unaligned and sits in bulk asset data, so it
cannot be a code pointer.

The 201 ROM-wide unresolved jump tables do not open a hole: MIPS jump tables
target labels *inside* a function rather than entry points, and any entry would
still have to appear as a data word — which neither address does.

**Residual gaps, both LOW:** runtime-arithmetic address construction would evade
both scans (compilers do not emit this for function pointers, and a leaf
diagnostic has no motive), and compressed ROM regions would hide references
(strongly mitigated by the positive control proving the GEV data is scannable).
What would settle it conclusively: an instrumented run logging entry to both
functions across a playthrough. Not judged necessary.

The empirical bound — fixtures spanning many boots with slot data intact — is
**consistent and corroborating, not load-bearing**.

### Superseded: notify + `.manual.bak`

**SUPERSEDED 2026-07-18** by the pak-write observation design above. Retained
for its reasoning, not as instructions. **Do not implement this.**

Neither of the two options above; a third that delivers
the dedicated slot's rollback semantics at roughly the host-snapshot's cost.

The earlier framing of this choice was a false dilemma — it compared the
dedicated slot's best case against a host snapshot assumed to be
one-time-at-enable, and "the snapshot ages badly" was an artifact of that
assumption rather than anything inherent. Nothing forces it to be one-time. The
host cannot distinguish an autosave flush from a manual one, but **the guest
can**, and it can say so.

**Mechanism:**

1. `RECOMP_PATCH` `func_8000B718_C318` (the live -> gamedata marshal; in `.main`,
   always resident, body fully decoded) to notify the host before doing its work.
2. `goemon_save_now()` calls that same function, so the autosave path brackets
   its own call with an in-progress flag and the patch suppresses the notify.
3. Host side, the notify arms a one-shot. When the next flush completes, the save
   file is copied to `.manual.bak`.

**The autosave path must actively DISARM, not merely decline to arm.** This is
not optional and does not follow from the flag alone. `func_8000B718_C318` has
**25 call sites**, not one (see `goemon_save_re.md`): a `jal` from the real save
routine plus 24 GEV script native-calls, and **half of those 24 are the RAM-only
suspend**, which marshals but never writes the pak. A suspend therefore arms the
one-shot and leaves it armed, because no flush follows. The next flush to arrive
may be an autosave — which would then be copied into `.manual.bak`, silently
replacing the rollback point with exactly the thing it exists to roll back from.

| sequence | outcome |
|---|---|
| manual save -> flush | `.manual.bak` written — intended path |
| suspend (no flush) -> autosave | autosave disarms; `.manual.bak` untouched |
| suspend -> manual save | still armed, flush follows — correct |
| suspend -> file erase/copy -> flush | `.manual.bak` holds post-operation state |

That last row is **correct behaviour, not a hole**, and is stated here so nobody
later reads it as one: the erase was the player's deliberate act, and every other
slot's last save survives in both files. But it is why the contract is worded as
below rather than as "last manual save".

**Contract:** `.manual.bak` holds *the file state as of the last deliberate
**save-class** operation* — not "the last manual save". The looser wording is the
honest one given the flush sources below.

**Flush sources — all 8, already mapped** (`jal func_80023610_24210`, the
0x500-slot write). The disarm rule's correctness depends on this set being
closed, and it is:

| sites | where | save-class? |
|---|---|---|
| 1 | `func_80214D58_5D0228` (`funcs_53`) | yes — the real save |
| 5 | `.file_15` Diary management (new / copy / erase) | yes — deliberate |
| 2 | `.main` pak **diagnostics** | **no** — they write an incrementing byte pattern to slot 0 |

With the set closed there is no mystery writer left to consume an armed one-shot.

**Open precondition — and its stakes are bigger than `.manual.bak`.** The two
diagnostic writers put an **incrementing byte pattern into slot 0**. If they are
runtime-reachable they destroy the live slot 0 save *directly* — autosave or not,
one-shot armed or not. Poisoning `.manual.bak` is the secondary casualty, and
scoping the risk to that undersells it; stakes determine how carefully someone
checks.

**Establish whether they are reachable at runtime.** If they are, this is a bug
in its own right, independent of autosave, and the disarm rule must cover them
as well.

**The empirical bound already in hand says they probably are not.** The fixtures
corpus spans many boots and play sessions with slot data persisting intact
throughout — had these writers fired in normal operation, slot 0 would have been
scribbled. So the static check is most likely confirming dead or menu-gated code
rather than hunting a live threat. Both halves belong in the check: the true
severity if reachable, and the evidence that it probably is not.

**Preconditions before the timer ships:**

1. The notify patch touches the **manual save path** for the first time. Confirm
   a manual save made through the patched `func_8000B718_C318` is byte-identical
   to the pre-patch baselines already in `docs/re-notes/fixtures/` — compare
   against `08`/`09`/`11`. Nearly free, and only because the corpus is versioned.
2. Resolve the diagnostics reachability question above.

**Why not the slot-write anchor.** Anchoring the notify on
`func_80023610_24210` instead is superficially better — "wrote the pak" is the
event whose consequence we snapshot, and suspends never reach it. It was checked
and rejected: it trades 25 fully-mapped call sites for 8 whose semantics had to
be re-derived, and requires reimplementing a pak-manager wrapper of unknown
complexity rather than a marshal body that is already fully decoded.

**Why not the dedicated slot.** Its one unique advantage is *self-service
restore* — a rollback point sitting in the file-select screen, loadable without
tools, where `.manual.bak` needs `adb` and a force-stop. That is real but not
gating: the precondition requires a rollback mechanism to **exist**, not to be
end-user-polished. The gap has a better closer already parked — the SAF
import/export item covers file-level restore and device migration together.

The disqualifying argument is the slot cost. Evaluating it as "it costs nothing
today, since 2 of 3 slots are in use" is personal-fork reasoning applied to a
repo with a parked **public-release** plan. For a public player using all three
files, the dedicated slot either destroys one silently or refuses permanently —
and a design whose acceptability depends on one developer's current slot usage
breaks precisely when the audience changes, which is the roadmap.

**The dedicated slot is therefore PARKED as a possible future opt-in**, and its
four open questions below stop gating anything. That is the practical payoff of
this decision.

### Dedicated-slot option — PARKED, not gating

Superseded as the rollback mechanism by the **pak-write observation design**
above — *not* by the notify design, which is itself dead and marked
do-not-implement. (This pointer named the notify design until 2026-07-19; it was
the fourth leftover-stale-cross-reference in this feature's history, which is
why the supersession chain is now spelled out rather than relative.) Kept because its
self-service-restore advantage may still justify it as an opt-in someday (though
SAF import/export may obsolete that too). **These four questions no longer block
anything** — they only matter if someone revives it.

Writing autosaves to a fixed slot instead of the cursor slot is a **one-line**
change (`patches/autosave.c` reads the cursor once, then passes `slot`
downstream). Its real benefit is *not* isolating other saves — it is preserving a
**rollback point**, since your manual save survives a bad autosave. Before
building it:

1. **Slot-collision guard.** With all three files in use, a fixed slot destroys a
   deliberately-made save with no warning — the exact failure this feature exists
   to avoid, and *worse* than the current design for that player. Needs: read the
   target slot back and refuse unless it is empty or already an autosave.
2. **Is "AUTO" expressible?** The displayed name lives in the payload, so it can
   be stamped in the image buffer after the memcpy and before the CRC (touching
   no live gamedata). Open: whether the name-entry charset admits it, and whether
   the name is validated on load.
3. **How is "empty" detected?** The guard is useless without it — an unwritten
   slot must be reliably distinguishable (`payload[0x6C] == 0` is the game's own
   test and is the obvious candidate). If it cannot be, the guard fails closed
   and the feature is inert on a fresh file.
4. **Is the payload slot-agnostic?** If the `0x304` block or the header write
   embeds a slot index anywhere, a save written to slot 2 but "belonging" to slot
   0 may load oddly. Probably clean; cheap to check.
5. **Configurable slot is the escape hatch, not polish.** A three-file player
   otherwise gets a permanent correct refusal and no recourse. Letting them
   nominate a slot converts a block into informed consent.

### Prerequisites for defaulting the timer to On

**Two independent gates, not one.** They protect different things:

1. The slot-collision story above — protects *other* saves.
2. The **save-data-settled check** — protects the *autosave itself*. A dedicated
   slot does nothing for a mid-transaction snapshot; a default-On timer writing
   half-applied state into the only autosave slot is still a bad autosave.

## Verifying it

> **While autosave is enabled, assume `.bak` contains autosave data.** The
> runtime rotates `current -> .bak` on every flush, and every save the feature
> makes is a flush, so the on-device `.bak` is an earlier *autosave*, not your
> last manual save. This applies to the current manual-trigger build, not just
> to a future timer — see "What it overwrites, exactly".
>
> `.manual.bak` now exists to be the deliberate rollback point (see "Decided
> design"), but it is **implemented and not yet verified on device**. Until that
> verification lands, **treat the `adb` backup below as the only trustworthy
> recovery copy.**

### 1. Back up the save first

The save is on app-scoped **external** storage, not internal app-private
storage. `MainActivity.onCreate` passes `getExternalFilesDir(null)/data` to
`nativeInit`, which becomes `APP_FOLDER_PATH`, so the full path is:

```
/sdcard/Android/data/com.goemon64.recomp/files/data/saves/mnsg.us.bin
```

**Running these from WSL:** native Linux `adb` sees no device (WSL2 has no USB
passthrough by default), but `adb.exe` works via WSL interop and is already on
the PATH. Either `alias adb=adb.exe`, or use `usbipd attach` to hand the device
to WSL for native `adb`. When using `adb.exe`, prefer explicit output paths —
it is a Windows process, so relative paths resolve against the Windows-side
interpretation of the working directory.

Because it is external storage, `adb pull` works directly — no `run-as`, and it
works on release builds too:

```sh
adb pull /sdcard/Android/data/com.goemon64.recomp/files/data/saves/mnsg.us.bin mnsg-backup.bin
ls -l mnsg-backup.bin      # expect 131072 bytes (0x20000)
```

Note that although this is not permission-protected, Android 11+ scoped storage
hides `Android/data` from third-party file managers and the stock Files app, so
a user without adb generally cannot reach it.

### 2. To restore — force-stop first, this step is not optional

```sh
adb shell am force-stop com.goemon64.recomp
adb push mnsg-backup.bin /sdcard/Android/data/com.goemon64.recomp/files/data/saves/mnsg.us.bin
```

Without the force-stop the restore is silently undone. The saving thread in
`librecomp/src/pi.cpp` owns that file and holds the save buffer in memory; on the
next guest write — or on exit, via `join_saving_thread` — it rewrites the whole
file from its own copy, straight over what you just pushed. You get no error,
just your old data back.

Backgrounding the app is **not** sufficient, and neither is swiping it from
recents: the process survives both. `am force-stop` is the only reliable way to
guarantee the saving thread is gone before you write the file.

Verify the restore took, after relaunching:

```sh
adb pull /sdcard/Android/data/com.goemon64.recomp/files/data/saves/mnsg.us.bin check.bin
cmp mnsg-backup.bin check.bin && echo "restore intact"
```

### 3. Watch the diagnostics

```sh
adb logcat | grep autosave
```

Three outcomes are distinguishable:

- `manual save -> status 0 (slot N)` — success
- `refused: no valid save slot selected (cursor N, need < 3)` — the slot guard
  fired
- `refused: unsafe state (mode .. dirty .. paused .. xfer .. lock .. sub ..)` —
  the safety gate fired, and the printed fields say **which** guard blocked it.
  This is the most useful on-device diagnostic for tuning the gate.

### 4. Success criterion

Status `0` is necessary but **not sufficient**. The read-back compare in step 9
only proves the pak write round-tripped — it cannot catch a divergence between
this reimplementation and the real `func_80214D58_5D0228` marshaling.

The only thing that validates the marshaling is a differential test:

1. At a fixed point in the game, save via `L + R + D-Pad Up`. Pull the file.
2. At the same point, save for real through a save NPC or inn. Pull that file.
3. Compare **the slot region only**, masking known-volatile fields.
4. Load each and confirm identical state.

**Do not `cmp` the whole file.** Two saves of identical state always differ in up
to `0xF0` header bytes, because step 10 leaks stack residue (see "How it works").
A whole-file compare reports a **false failure**. Use:

```sh
cmp -i 256 autosave.bin gamesave.bin      # skip the 0x100 header region
```

**The volatile mask**, measured from two consecutive autosaves 2.7s apart:

| offset | field |
|---|---|
| slot base `+0x00`–`+0x03` | the slot's CRC word |
| payload `+0x264` (observed at `+0x267`) | play-time counter |

Anything differing outside that mask is a genuine divergence.

Because a save NPC writes to the **same slot** the autosave does, one spot yields
both saves through both code paths — the cleanest possible comparison. Note the
runtime keeps a one-generation `.bak`, so if both saves happen before you pull,
`mnsg.us.bin` is the second and `mnsg.us.bin.bak` is the first.

The save files these results came from are committed under
`docs/re-notes/fixtures/`, with a per-file provenance map. Every comparison
below reproduces from them directly.

#### Result (2026-07-18): PASSED

Slot 0 differed in exactly 5 bytes — `0x100`–`0x103` (CRC) and `0x36B`
(play-time) — i.e. precisely the mask. The `0x304` payload is byte-identical
between this reimplementation and the game's own routine. Corroborated by two
in-game saves back-to-back producing the *same* 5-byte signature, and by header
bytes `0x00`–`0x0F` being identical across both writers with the first
difference at exactly `0x10`.

#### Sharpened result (2026-07-18): PASSED

The result above compared two saves of essentially identical state — an "equal
when nothing changed" outcome. It has since been repeated **under changed
state**, which is the version that actually validates the marshaling.

After a play session in which the player took damage and spent money, an
autosave and an in-game NPC save at the same point differed in the slot region
by exactly the mask — `0x100`-`0x103` and `0x36B` — and nothing else. Four
payload fields had moved against the previous baseline, and **both writers
recorded all four identically**:

| payload | field | change |
|---|---|---|
| `+0x70` | current HP | `11 -> 10` |
| `+0x74` | ryo | `533 -> 363` |
| `+0x7C` | unidentified counter | `0 -> 2` |
| `+0x264` | play time | advanced |

Fixtures `10-autosave-statechange.bin` / `11-npcsave-statechange.bin`; ordering
established from the monotonic play-time counter rather than assumed.

**Watch out for `+0x6C`.** It is *max* HP, not current, so taking damage does not
move it — picking it as the field to exercise makes a real state change look like
no change at all. Current HP is `+0x70`.

#### The lesson worth carrying forward

The header divergence was initially escalated as a defect and was not one. What
made the process work was not the differential test existing, but its criterion:
**explain every differing byte or do not wave it through**. Status `0` plus a
matching payload was available as a stopping point and would have shipped an
unexamined `0xF0`-byte delta. The next divergence will also hide behind a
plausible aggregate number — and sometimes the outcome of that discipline is an
explanation rather than a bug.

### 5. Verifying the `.manual.bak` rollback point

**RUN AND PASSED on device, 2026-07-19.** Results under each step below. This
was the gating precondition for the timer, and it is now met: a rollback
mechanism exists that does not depend on `.bak`, and it is verified rather than
merely implemented.

**A null result nearly read as a pass, and then as a failure — note the shape.**
The first attempt at step 1 found no `.manual.bak` and looked like a defect. The
device was simply running an APK seven hours older than the feature. **Absence
of `.manual.bak` looks identical whether the feature is broken or not
installed** — so check `adb shell dumpsys package com.goemon64.recomp | grep
lastUpdateTime` against the build time before drawing any conclusion from a
missing file.

```sh
SAVES=/sdcard/Android/data/com.goemon64.recomp/files/data/saves
```

1. **A deliberate save produces the rollback point.** Save through an NPC or
   inn, then pull both files and confirm they match exactly:

   ```sh
   adb pull $SAVES/mnsg.us.bin main.bin
   adb pull $SAVES/mnsg.us.bin.manual.bak manual.bin
   cmp main.bin manual.bin && echo "rollback point matches the manual save"
   ```

   A whole-file `cmp` **is** correct here — unlike the differential test, both
   sides are copies of the same flush, so the step-10 stack residue is identical
   rather than divergent.

   **PASSED (2026-07-19).** `.manual.bak` byte-identical to the manual save.
   Also confirmed **not stale**, which a bare existence check would have missed:
   it differed from the pre-install save pulled minutes earlier, so the one-shot
   armed and fired on *this* save rather than a leftover file passing inspection.
   Corroborated by `.bak` being byte-identical to that pre-install save — three
   files telling one consistent story, which is stronger than any single compare.

2. **Autosaves do not disturb it — the closing bracket on this whole arc.** With
   the rollback point in place from step 1, fire the combo **twice, ~3 seconds
   apart**. That is deliberately the same 2.7-second sequence that first
   demonstrated the loss, back when it rotated the last manual save off the
   device entirely. Then:

   ```sh
   adb pull $SAVES/mnsg.us.bin.manual.bak manual-after.bin
   cmp manual.bin manual-after.bin && echo "rollback point survived two autosaves"
   ```

   `.bak` should now hold an *autosave* (an autosave-of-an-autosave, as before —
   that behaviour is unchanged and is not a defect), while `.manual.bak` still
   holds the manual save. **Re-running the original loss scenario and having it
   pass is the point of the exercise**, not an incidental check.

   **PASSED (2026-07-19).** Two autosaves 4.6s apart, both `status 0`.
   `.manual.bak` byte-identical to its step 1 content; `.bak` differs from the
   manual save, i.e. it had become autosave A exactly as it always did. The loss
   scenario reproduced faithfully and the rollback point survived it.

   **Getting there took a refusal worth recording.** The first attempt fired the
   combo while standing in an inn and was refused twice — every guard passed
   except `sub 3`. See "Submode 3" below; it is the gate working, not a defect.

3. **Confirm it is loadable, not merely byte-identical.** Force-stop, push
   `.manual.bak` over the main save, relaunch, and confirm the game loads the
   manual-save state:

   ```sh
   adb shell am force-stop com.goemon64.recomp
   adb push manual.bin $SAVES/mnsg.us.bin
   ```

   The force-stop is **not optional** — see step 2 of "Verifying it" for why a
   restore without it is silently undone.

4. **Confirm the failure paths log.** `stderr` is pumped into logcat
   (`android_redirect_stdio_to_logcat` in `src/main/main.cpp`), so the existing
   `adb logcat | grep autosave` picks up the copy-failure message alongside the
   refusal diagnostics.

   **CONFIRMED** — the refusal diagnostics carried the whole of step 2's
   troubleshooting. No copy-failure message was produced, which is the expected
   silence rather than an untested path.

#### Submode 3 — the gate refuses in inns, and that is faithful

`0x800C7AA4` semantics remain an **open question** (`goemon_save_re.md:201`,
MEDIUM; listed at line 270). This run added one concrete data point against it:

**OBSERVED: submode reads 3 while standing in an inn**, with every other guard
passing (`state 13 | phase 1 | done 0 | paused 0 xfer 0 lock 0 | busy 0 loading
0 gA 0 gB 0 | file11 1`), so the gate refuses there.

**OBSERVED: the game's own pause menu does not open in that same spot.** This is
the check that matters, and it was run deliberately: the entire gate is a
re-evaluation of the game's pause-menu predicate, so if submode 3 blocked *us*
but not the game, our guard would be stricter than the game's and autosave would
be silently refusing in states the game considers safe. It is not. The guard is
faithful.

**NOT established:** what submode 3 *means*. "Inn interior", "post-save", "shop"
and others all remain live. An earlier reading of this same refusal as "still in
dialogue" was **wrong** — the player was standing still in an inn, not talking
to anyone — and is recorded here because the plausible-sounding guess was
offered before the cheap experiment that actually settled the faithfulness
question.

Practical consequence: **autosave will not fire inside inns.** Harmless today,
since inns are themselves save points, but it is a real coverage limit that the
timer inherits, and it should be understood before the timer defaults to On.

### 6. Also confirm

- ~~The save slot count~~ — **done**, confirmed 3 (see "Slot guard").
- The setting appears in General, toggles, and persists across a restart
  (settings are written on menu *close* — see `close_config_menu_impl` in
  `src/ui/ui_config.cpp`).
- No write occurs during cutscenes, dialogue, the pause menu, area transitions,
  or the Impact/sidescroller stages.
- After any `assets/` change, delete the device's `files/data/.assets_version`
  stamp or the app keeps the OLD extracted UI.
