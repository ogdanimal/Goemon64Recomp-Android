# Autosave verification fixtures

Controller Pak save files pulled from the test device (Retroid Pocket 5) during
the autosave verification session, 2026-07-18. Every load-bearing claim in
`docs/autosave.md` and `docs/re-notes/goemon_save_re.md` traces to these exact
bytes; this directory exists so the notes can **cite** evidence rather than
describe it.

These are **immutable historical evidence**, not backups. They are distinct from
the live recovery copy a tester pulls at the start of each session — that one is
transient, machine-local, and regenerated per the procedure in
`docs/autosave.md`. Do not overwrite anything here with a fresh capture; add a
new numbered file instead.

Each file is a whole `mnsg.us.bin` (131072 bytes = `0x20000`). Pak layout is a
`0x100` header region then `0x500` slots at `0x100 + slot * 0x500`.

## Provenance

| file | sha256 (first 8) | what produced it |
|---|---|---|
| `01-manual-prior.bin` | `64eb2706` | device `.bak` at session start — oldest state preserved |
| `02-manual-baseline.bin` | `5b746485` | device `mnsg.us.bin` at 16:19, before any autosave existed on device |
| `03-manual-1634.bin` | `e1be2627` | ordinary in-game save at 16:34 |
| `04-autosave-A.bin` | `8f48032d` | **autosave** via `L+R+D-Up`, 17:03:22 |
| `05-autosave-B.bin` | `0dd9151b` | **autosave**, 17:03:24 — 2.7s after `04`, same spot |
| `06-autosave-C.bin` | `18d669f2` | **autosave**, 17:09:33 (slot 0) |
| `07-npcsave-cross.bin` | `0b2a8a94` | **in-game NPC save** shortly after `06`, same slot |
| `08-npcsave-consecutive-A.bin` | `82e80f11` | **in-game NPC save**, first of two back-to-back |
| `09-npcsave-consecutive-B.bin` | `eed7fade` | **in-game NPC save**, second of the pair, nothing in between |
| `diary-slot-count.png` | — | "Select Adventure Diary" screen |

Two files were captured twice under different names in the working directory and
are stored once here. **Both identities are themselves evidence of the `.bak`
rotation** (`librecomp/src/files.cpp:39-45`, `current -> .bak` on every flush):

- `02` was also the device's `.bak` after `03` was written — i.e. the 16:34 save
  rotated the 16:19 one out of `mnsg.us.bin`.
- `06` was also the device's `.bak` when `07` was pulled. This **confirms rather
  than assumes** that the cross-writer comparison below is autosave-vs-NPC-save:
  the NPC save rotated the 17:09 autosave into `.bak`, where it was recovered.

## Which finding each pair supports

All commands verified to reproduce from these files as committed.

### The volatile-field mask — `04` vs `05`

Two autosaves 2.7s apart at the same spot.

```sh
cmp -l 04-autosave-A.bin 05-autosave-B.bin
# -> 0x100 0x101 0x102 0x103 0x36B, and nothing else
```

`0x100`-`0x103` is slot 0's CRC word; `0x36B` is payload `+0x267`, the low byte
of the play-time counter at `+0x264`. **Zero header bytes differ** — the autosave
path's stack residue is deterministic because it is always called from the same
point in the per-frame patch. Cited by `docs/autosave.md` § "Success criterion".

### Cross-writer equivalence — `06` (autosave) vs `07` (NPC save)

The differential test. Compare the slot region only:

```sh
cmp -i 256 06-autosave-C.bin 07-npcsave-cross.bin
# slot-region differences are exactly 0x100-0x103 and 0x36B — the mask above
```

The `0x304` payload is byte-identical between the reimplementation and
`func_80214D58_5D0228`. **This is the feature's marshaling validation.**

**Do not `cmp` these whole-file** — up to `0xF0` header bytes always differ (see
below) and a whole-file compare reports a false failure.

The real header content does match:

```sh
cmp -n 16 06-autosave-C.bin 07-npcsave-cross.bin   # identical
```

Bytes `0x00`-`0x0F` are the only initialised header content; the first differing
byte is exactly `0x10`. Cited by `goemon_save_re.md` § "The header write leaks
stack".

**Caveat on strength:** both saves captured near-identical game state, so this is
an "equal when nothing changed" result. The sharper test — change hearts (`+0x6C`),
ryo (`+0x74`) or stage id (`+0x200`) first — is listed as step 2 in
`RESUME-autosave.md` and has **not** been run. When it is, add the pair here.

### The game's own header signature — `08` vs `09`

Two in-game saves back-to-back, nothing in between.

```sh
cmp -l 08-npcsave-consecutive-A.bin 09-npcsave-consecutive-B.bin
# slot: 0x100-0x103, 0x36B (same 5-byte signature as the autosave path)
# header: 15 bytes at 0x11-0x13, 0x15-0x17, 0x1D-0x1F, 0x48-0x4B, 0x7E-0x7F
```

Two distinct facts:

1. The game's own path produces the **same** slot signature as ours —
   independent corroboration of the mask.
2. The game churns **15 header bytes** where our path churns 0. This was
   initially escalated as a defect; it is not one. All 15 lie at `>= 0x10`,
   inside the uninitialised stack residue, read by nothing. The difference is
   only that the save-menu overlay varies that stack region between calls while
   the per-frame patch does not.

### `SAVE_SLOT_COUNT = 3` — `diary-slot-count.png`

Three entries, the third reading "From the beginning" (empty). Closes the last
MEDIUM-confidence assumption in the feature. Note the count is visible on the
**file-select** screen — saving through an NPC offers no slot choice.

The screenshot also shows heavy orange/green vertical banding, an unrelated
rendering artifact worth its own investigation.

## Baselines for the next change

If the header handling is ever changed, these are the baselines to validate
against, and the success criterion is **signature-matching, not zero diff**:
after such a change, two autosaves at different times *should* churn the same
15 header bytes `08`/`09` show, because several are time-like. Carrying today's
"autosaves leave the header untouched" expectation forward would make a correct
change look like a regression.
