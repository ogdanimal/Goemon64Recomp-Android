# RE notes — index and STANDING WARNINGS

Read this file before trusting anything in `goemon_*.md`. The warnings below are
methodology failures that have already produced wrong conclusions in these notes,
each one caught only after it had cost time. They generalise; the specific
corrections they triggered are recorded in the individual files.

---

## STANDING WARNINGS

### 1. A `jal`-only scan produces FALSE NEGATIVES. Any negative conclusion resting on one is suspect.

Overlays never reach base-exe code by `jal`. They use `lui $t9, <hi> / addiu $t9, $t9, <lo> / jalr $t9`.
A scan that greps for `jal <addr>` therefore sees **none** of the overlay call
sites, and "no callers found" reads as "dead code" when the function may have
dozens of live callers.

Damage this actually did (`goemon_default_cam_writer.md`):
- `func_80012878` was recorded as having "no jal callers … or unused". It has
  **22 indirect sites across 13 area overlays**, and it is the blend-from-live
  path that forbids storage-side camera rotation. Concluding it was dead nearly
  led to an architecture change that would have broken every scripted camera
  move in those overlays.
- `func_80012900` was recorded as having "exactly TWO jal callers". It has **63**.

How to scan properly: search for the `addiu` that forms the low half, using the
SIGNED 16-bit form of the low halfword (e.g. `0x8000F420` → `addiu … -0xBE0`,
`0x80012878` → `addiu … 0x2878`), then confirm the paired `lui` and a following
`jalr`. Beware sign: `0xF420` is `-0xBE0`, not `0xF420`.

### 2. Validate a scan pattern against a KNOWN POSITIVE before trusting a zero.

A zero result means either "no hits" or "my pattern is broken", and the two are
indistinguishable. On 2026-07-19 a caller count returned `0 / 0 / 0` purely
because the grep assumed single spaces where the recompiled output uses several
(`addiu       $t9, $t9, -0xBE0`). The corrected pattern returned 130.

Always run the pattern against a case known to have hits first. This is the same
family as the rule below about diagnostics — silence is not evidence.

### 3. For anything that refuses/declines/reports-nothing, build the diagnostic that proves it REACHED its decision point.

Earned three separate times on the autosave feature (see `../autosave.md`), then
again on the analog camera. A passing test with no such diagnostic proves nothing,
because an inert check and a working one look identical from outside.

### 4. Identify a struct field by HOW ITS TRIG IS CONSUMED, not by an inherited label.

Camera `+0x18` and `+0x1A` are adjacent `s16` fields (`unknown_18` / `unknown_1a`
in `patches/types.h`), and two separate investigations labelled `+0x1A`
inconsistently ("roll/twist" vs "fov"). The consumption pattern settles it:

- `+0x18` → the view builder takes **sin and cos separately** to build an
  up-vector `(sin r, cos r, 0)` ⇒ an ORIENTATION angle (roll). The analog
  camera's rotated copy zeroes this, correctly.
- `+0x1A` → the projector `func_8001CB40` passes it to `func_80003DC0_49C0`,
  which returns a **ratio of two trig values** (a tangent) ⇒ a PROJECTION SCALE
  (fov half-angle). **Never zero this.** Zeroing it would collapse the
  projection. `func_80012878` seeding a tween channel from `+0x1A` is therefore
  a zoom ease, not a roll ease — soft corroboration.

Separate sin/cos ⇒ orientation. Their ratio ⇒ projection scale.

### 5. Trig angle convention: full turn = 0x400, NOT 0x10000.

`func_80003E10_4A10` (math_sin) returns a true unit sine but its full turn is
**0x400** (1024). Code assuming the 0x10000 binang convention and adding `0x4000`
for cosine aliases sin and cos to the SAME table entry (a whole number of
periods), producing a non-orthonormal matrix that silently rescales rather than
rotates. Proof: `widescreen.c` masks its argument `& 0x3FF`. Convert with
`(angle >> 6) & 0x3FF`; a quarter turn is `0x100`.

---

## Camera consumers and which are handled

The analog camera never mutates the game's Camera; it hands a rotated private
copy to specific consumers. Any consumer NOT in the handled list silently reads
the UNROTATED camera and disagrees with the rendered view. See
`goemon_default_cam_writer.md` §6(3) for why per-consumer hooks are the only
viable architecture — and do not re-open that question.

HANDLED (all in `patches/camera.c`):
- `func_80017D8C_1898C` — world render, via `apply_analog_camera()`
- `func_801CE4D0_58A3E0` — movement basis, via the `func_801CE3F0` pointer swap
- `func_801F8670_5B4580` — skybox scroll, via `func_801F8644` **and**
  `func_802242DC` (two independent callers; the second passes data-driven
  panorama dimensions and is a real path, not a symbol artifact)
- `func_8000FE1C_10A1C` — positional audio pan, via the `func_8000F6E8`
  argument substitution

KNOWN UNHANDLED:
- **World→screen projector** `func_8001CB40` (0xF8) / `func_8001CC38_1D838`
  (0x114) — **PARKED 2026-07-19. Do not start without a confirmed symptom.**
  Converts a world point to a screen pixel using the camera's `eye−look_at`
  basis, `+0x1A` (fov, via a tangent) and the viewport at `camera+0x40`. Reads
  the unrotated camera, so it computes pixels for a view we are not rendering;
  anything drawn there lands wrong, by an error that grows with how far the
  player has orbited. 11 direct call sites across 9 files plus 1 indirect;
  `func_8000F468_10068` and siblings call it and the audio panner back to back
  (combined "draw at a projected world point + play a positional sound there").
  WHY PARKED, not just unfinished: no thin forwarder exists, so the fix means
  reproducing ~65 instructions of dense float math (normalize / basis transform
  / tangent / viewport) — and a transcription slip there would misplace
  everything it draws rather than fail loudly. We know the mechanism is wrong
  but NOT that anything a player sees routes through it in normal play.
  The `+0x1A` question is NOT a gate: `memcpy` carries the field through
  verbatim, so the fix needs no decision about it (see warning 4).
  UNPARK TRIGGER: a 2D effect observed sitting beside rather than on whatever
  produced it, after orbiting. INFERRED (not observed): a point exactly at
  `look_at` projects to screen centre under both cameras, so error should be
  near zero for effects on the player and grow with distance from him — look
  preferentially at effects some way from Goemon.
- **Camera-facing billboard basis** `func_08000394_6AF7A4`, in overlay
  `.file_27` (rom `0x6AF410`, vram `0x08000000`). Which area loads it is
  unresolved, so it is not testable as written.
- **Four direct callers of `func_8000FE1C`** that bypass the `func_8000F6E8`
  hook: `0x801F2A58`, `0x801F2A7C` (`funcs_34.c`), `0x8000FE04` (`funcs_11.c`),
  `0x80210074` (`funcs_87.c`). Closing them means reproducing all 0x360 bytes of
  `func_8000FE1C`; revisit only if a specific sound is audibly mispanned while
  the rest are correct.
- Minor, listed so they are not re-chased: a camera-direction spatial probe
  (`func_801EAF50_5A6E60`) and camera→object distance for LOD/fade
  (`func_08000F40_713EB0`, `func_08001294_714204`). The rotation is
  radius-preserving about `look_at`, so distance error is small and zero at the
  player.

---

## Area transitions

Current map id = `*(volatile u16*)0x800C7AB2`; previous = `0x800C7ABC`; pending
destination = `0x800C7CA0`. The area-change commit `func_8000B364` copies
current→previous then installs the destination. Map ids are per-room
(0..0x26C across only 14 stages) and **house interiors do get their own id** —
device-confirmed 2026-07-19 via the `[acamM]` diagnostic (361↔418 walking in and
out of a building).

`gamedata+0x200` is NOT a live area id — it indexes the continue/save-point
table at `0x8005BA30` and only moves when you save. That explains its
long-standing "no resolvable writer" mystery in `goemon_save_re.md`.

---

## File index

- `RESUME.md` — analog camera resume prompt (v14 + later fixes)
- `RESUME-autosave.md` — autosave resume prompt
- `goemon_default_cam_writer.md` — camera architecture; **carries corrections, read the warnings above first**
- `goemon_global_camstate.md` — camera-adjacent globals
- `goemon_basis_verify.md` — movement-basis 1:1 verification
- `goemon_player_pos.md` — player position/heading
- `goemon_math_helpers.md` — trig helpers (see warning 5)
- `goemon_save_re.md` — save format/routine RE
- `goemon_cheats_re.md`, `goemon_character_swap_re.md`, `goemon_overlay_scan.md`,
  `goemon_trace_0800037C.md`, `goemon_critic.md`, `vulkan_device_floor.md`
- `fixtures/` — evidence corpus; cite it rather than re-deriving
