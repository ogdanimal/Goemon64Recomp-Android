# Goemon64Recomp — Overlay camera-writer enumeration & shared-patch-point verdict

Static scan of all 148 `RecompiledFuncs/funcs_*.c` (2,859 RECOMP_FUNCs, of which 2,209 are
0x08-segment overlay functions). Scripts used (all in this scratchpad, run inside WSL):
`scan_swc1.py` (offset histograms), `cluster_skeletons.py` (mnemonic-skeleton similarity vs
target), `camera_callers.py` (helper-caller enumeration + greedy clustering of all
0x8021927C callers), `trace_stores.py` (per-store base-pointer provenance),
`classify_writers.py` (provenance classification of every overlay float-writer).

Counting note: the first-pass histogram counts raw `swc1` comment lines; the recompiler
duplicates delay-slot comments, so raw counts are slightly inflated (target: raw eye=17/at=26,
address-deduplicated eye=17/at=22). Dedup counts are used everywhere below.

---

## 1. Full candidate list (swc1 histogram, eye = stores @0x0/0x4/0x8, at = @0xC/0x10/0x14)

Threshold eye>=3 & at>=3 & total>=10 — 70 functions (raw counts, sorted by total).
Top 20 shown; full table reproducible via `scan_swc1.py`:

| func | file | eye | at | total | segment |
|---|---|---|---|---|---|
| func_08000AAC_73523C | funcs_142.c | 56 | 83 | 139 | overlay (rombase 0x734790) |
| func_08000AC0_726F50 | funcs_139.c | 27 | 53 | 80 | overlay (0x726490) |
| func_8021C744_5D7C14 | funcs_40.c | 39 | 27 | 66 | base-exe |
| func_8020FEA0_6978A0 | funcs_98.c | 42 | 22 | 64 | base-exe |
| func_08005790_6F8C70 | funcs_126.c | 17 | 46 | 63 | overlay (0x6F34E0) |
| func_08000B54_72EDB4 | funcs_140.c | 17 | 27 | 44 | overlay (0x72E260) |
| **func_0800037C_72E5DC** | funcs_140.c | 17 | 26 | 43 | overlay (0x72E260) — the target |
| func_801D15F0_58D500 | funcs_30.c | 21 | 11 | 32 | base-exe |
| func_801CDD10_5F90F0 | funcs_62.c | 9 | 23 | 32 | base-exe |
| func_080040B8_6C3808 | funcs_107.c | 8 | 23 | 31 | overlay (0x6BF750) |
| func_08002414_715384 | funcs_134.c | 14 | 16 | 30 | overlay (0x712F70) |
| func_08004000_6B72A0 | funcs_104.c | 9 | 17 | 26 | overlay (0x6B32A0) |
| func_801E6348_655198 | funcs_83.c | 4 | 22 | 26 | base-exe |
| func_080002B0_711DA0 | funcs_134.c | 7 | 14 | 21 | overlay (0x711AF0) |
| func_0800107C_72C8CC | funcs_140.c | 13 | 8 | 21 | overlay (0x72B850) |
| func_0800314C_6CB35C | funcs_112.c | 6 | 14 | 20 | overlay (0x6C8210) |
| func_08001954_6F1B64 | funcs_125.c | 5 | 13 | 18 | overlay (0x6F0210) |
| func_08001464_70B4A4 | funcs_131.c | 4 | 13 | 17 | overlay (0x70A040) |
| func_0800624C_6C599C | funcs_110.c | 5 | 11 | 16 | overlay (0x6BF750) |
| func_08003168_6EC2B8 | funcs_122.c | 8 | 8 | 16 | overlay (0x6E9150) |

**Important caveat discovered by provenance tracing (section 4): a raw eye/at histogram
badly conflates three unrelated things** — (a) genuine Camera eye/at writes, (b) writes to
the camera *params* block at `*(obj+0x84)` (its +0x8/+0xC/+0x10 fields land in the same
offset window), and (c) arbitrary Vec3f array writes (particles, paths). The base-exe high
scorers are false positives: `func_8021C744_5D7C14` initializes an array stepping a pointer
by +0x10 (CONFIRMED from its store bases: `addiu reg,reg,0x10` chains into a 0x8023xxxx
data table); `func_8020FEA0_6978A0` writes fields at struct+0x5C of one object;
`func_801D15F0_58D500` / `func_801CDD10_5F90F0` / `func_801E6348_655198` write through
stack-passed argument pointers at non-camera offsets (0x20/0x28/0x54/0x5C...) — generic
utilities, not the camera engine.

## 2. Cluster verdict — is the target copied per area?

**NO per-area copies exist.** Skeleton comparison (mnemonic-sequence difflib ratio) of the
502-instruction target against **all 2,209 overlay functions** found exactly ONE function
above 0.55: `func_08000B54_72EDB4` (ratio 0.778). Every other overlay function scores <=0.25
(next best: `func_080004F8_71AA08` at 0.245). Greedy clustering (>=0.70) of all 100 overlay
callers of the params-attach helper 0x8021927C yields 38 clusters; the target's cluster
(#36) has exactly 2 members:

- `func_0800037C_72E5DC` (vram 0x0800037C) — 502 instrs, eye=17/at=22
- `func_08000B54_72EDB4` (vram 0x08000B54) — 498 instrs, eye=17/at=22

Both have inferred overlay ROM base `ROM - (vram - 0x08000000)` = **0x72E260 — the SAME
overlay image**. They are two variants of the 7-case camera state machine living side by
side in one per-area overlay (vram delta = ROM delta = 0x7D8), presumably two camera modes
for that area, not per-area clones.

CONFIRMED structural identity of the pair (evidence):
- Same 7-case `jr`-table state machine dispatched on `*(u8*)(*(a0+0xD0)+0)`.
- Identical resolved-callee sequence (34 vs 32 jalr sites); the sibling adds exactly two
  calls not in the target: `0x80023DF0` (jalr@0x08000BEC) and `0x80023E40` (jalr@0x080010F8)
  = `func_80023DF0_249F0` / `func_80023E40_24A40`, both swc1-free (checked).
- Identical store idiom (section 4): case-0 defaults through `*(s1+0x84)`
  (sibling: swc1@0x08000BB8/0x08000BCC/0x08000BE0 vs target 0x080003E0/0x080003F0/0x08000400),
  mirrored global writes through `*(0x801FC614)` / `*(0x801FC61C)`
  (`lui 0x8020 / addiu -0x39EC|-0x39E4`), and four 6-store eye+at blocks through
  `*(s0+0x14)`.

The other 36 clusters are unrelated per-area object handlers (spawners, actors) that merely
call 0x8021927C once to attach the params pointer — 97 of the 100 callers write zero or
near-zero eye/at floats.

**How many distinct overlay camera routines exist?** Classifying every overlay function by
store-base provenance (`classify_writers.py`), the "writes a camera object through the
state block at `*(a0+0xD0)`" idiom (cam_state) appears with full eye+at coverage in only a
handful of functions, each a DIFFERENT implementation (pairwise skeleton ratios <0.25):

| func | rombase (overlay) | cam_state stores (eye/at) | note |
|---|---|---|---|
| func_08000AC0_726F50 | 0x726490 | 48 (19/29) | biggest camera routine in the game, 1467 instrs; Camera ptr at `*(s0+0x1C)` (not +0x14); also writes params (+0x84) and the same 0x801FC614/61C globals |
| func_0800037C_72E5DC | 0x72E260 | 30 (14/16) | the target |
| func_08000B54_72EDB4 | 0x72E260 | 30 (14/16) | the sibling variant |
| func_08000AAC_73523C | 0x734790 | 9 (3/6) + 122 sp-cached | huge writer, pointers cached in stack slots; partially camera, partially other Vec3f work (INFERRED) |
| func_08001464_70B4A4 | 0x70A040 | 6 (0/6) | at-only adjuster |
| func_08001980_718820 | 0x716EA0 | 6 (3/3) | small full writer |
| func_08000DA0_6AE160 / func_080000D8_6AD498 / func_080007A8_6ADB68 | 0x6AD3C0 | 4 (4/0) each | eye-only partial writers |
| func_08001460_702560 | 0x701100 | 4 (2/2) | partial |
| func_08001CE4_70FD94 | 0x70E0B0 | 4 (4/0) | partial |
| func_0800046C_73D83C | 0x73D3D0 | 4 (2/2) | partial |
| func_080002B0_731560 / func_08000644_726AD4 / func_08000410_710A70 / func_080009CC_6DA89C | 0x7312B0 / 0x726490 / 0x710660 / 0x6D9ED0 | 3 each | partial |

So: **1 routine family = 2 copies (both in overlay 0x72E260); every other camera writer is a
distinct per-area implementation**, roughly 6 substantial ones plus ~10 partial adjusters,
spread over ~12 overlays. The heavy sp-based writers (`func_08005790_6F8C70` 48 sp-stores,
`func_08002414_715384`, `func_0800107C_72C8CC`, etc.) do not exhibit the state-block camera
idiom and are most plausibly path/effect Vec3f fillers (INFERRED, not verified per-function).

## 3. Shared write point — verdict

**There is NO base-exe function that writes Camera eye/at on behalf of the overlay
routines. The eye/at math is fully inlined in each overlay routine.** Evidence:

- The pair's complete callee set (23 distinct base-exe functions) was extracted and grepped
  for float stores. Callees with zero `swc1`: 0x8021927C (params attach:
  `sw $t6,0x84($a0)` of `*(u32*)0x8015CD60`, disasm @0x80219288), 0x802192B4 (bounded RNG),
  0x8021804C (callback registration — dispatches to 0x802171A8 with table 0x802139E0
  count 13 or 0x80211EF8 count 8, disasm @0x80218058-0x8021807C), 0x80221FB0 / 0x80221ED4
  (flag clears), 0x801DACDC (arg-massaging wrapper -> 0x801DAD68, sound-effect-like),
  0x80038BC8, 0x80031790, 0x80014B74 (linked-list unlink), 0x80034ED4 (wrapper),
  0x8003D388, 0x80024038, 0x8003F1D8, 0x80012900, 0x80023DF0, 0x80023E40.
- Callees that DO contain swc1 store only to sp spills or arg offsets >=0x1C — never
  0x0..0x14: 0x800124A0 (`swc1 $f12,0x70($a0)`), 0x800124AC (0x78/0x20), 0x80012518
  (0x40/0x48/0x50/0x98/0xA0/0xA8), 0x80012628 (0x58/0x60/0x68/0xB0/0xB8/0xC0), 0x80012818
  (0x8C..0xC4), 0x800125F0 (single `swc1 $f12,0x4($t9)` — global field, not an eye trio),
  0x801DAD68, 0x802171A8 (sp only). These are the per-object math/param helpers.
- The actual render-Camera writes in the pair are four inline 6-store blocks through
  `*(s0+0x14)` where `s0 = *(a0+0xD0)`; offsets 0x0/0x4/0x8 (eye) + 0xC/0x10/0x14 (look_at)
  exactly match the validated Camera layout. Target block addresses: 0x080008B4-0x080008FC,
  0x08000988-0x080009D8, 0x08000A0C-0x08000A50, 0x08000A94-0x08000AE8 (e.g.
  `0x080008B4: swc1 $f4,0x0($t4)` with `$t4 <= lw 0x14($s0) <= lw $s0,0xD0($a0)`).
  Sibling blocks: 0x080010A4-0x080010EC, 0x08001160-0x080011AC, 0x080011E0-0x0800122C,
  0x08001260-0x080012B0.
- Remaining 0x0..0x14 stores in the pair are NOT the Camera: 3 params defaults via
  `*(s1+0x84)` (+0x8 overlay float, +0xC=434.0f, +0x10=5.0f), 9 via `*(*(s0+0x4)+0x18)`
  (+0x8/+0xC/+0x10 of a linked object), 6 via the globals `*(0x801FC614)` / `*(0x801FC61C)`
  (+0x8/+0xC/+0x10, mirrored writes).

**Practical patch consequences:**
- A complete input-side hook of THIS overlay's camera = patch **2 functions**
  (`func_0800037C_72E5DC` + `func_08000B54_72EDB4`), both in funcs_140.c, same overlay.
- A complete input-side win across ALL special-camera areas = hook every distinct writer:
  at minimum the 6 substantial cam_state writers (0x726F50, the 0x72E260 pair, 0x73523C,
  0x718820, 0x70B4A4) and up to ~16 including partial adjusters — each with different code,
  so no single signature patch applies.
- The only true single choke point remains **consumer-side**: the Camera struct itself
  (pointer reachable as `*(*(camObj+0xD0)+0x14)` in the target overlay, `+0x1C` in overlay
  0x726490 — per-overlay layout, so not universal either) and the render-side view builder
  `func_80017D8C_1898C` (guPerspective+guLookAtHilite), which is exactly what the deployed
  v9 render-rotation patch hooks. This scan confirms v9's patch point is the ONLY
  overlay-independent one.

## 4. Overlay/area attribution (from ROM offsets)

Inferred overlay ROM base = ROM − (vram − 0x08000000). Overlays containing camera-object
writers (no area names recoverable statically from RecompiledFuncs alone; bases identify
distinct overlay images in ROM order):

| rombase | funcs files | camera writers | weight |
|---|---|---|---|
| 0x6AD3C0 | funcs_100.c | 3 eye-only partials | light |
| 0x6D9ED0 | funcs_117.c | 1 partial | light |
| 0x701100 | funcs_129/130.c | 1 partial | light |
| 0x70A040 | funcs_131.c | 1 at-only | light |
| 0x70E0B0 | funcs_133.c | 1 partial | light |
| 0x710660 | funcs_133.c | 1 partial | light |
| 0x716EA0 | funcs_135.c | 1 full (6 stores) | medium |
| 0x726490 | funcs_138/139.c | func_08000AC0_726F50 (48) + 1 partial | HEAVY |
| **0x72E260** | **funcs_140.c** | **the target pair (30 each)** | HEAVY |
| 0x7312B0 | funcs_141.c | 1 partial | light |
| 0x734790 | funcs_142.c | func_08000AAC_73523C (9 + sp-heavy) | medium |
| 0x73D3D0 | funcs_144.c | 1 partial | light |

Note funcs_NNN.c files each contain functions from multiple overlays (e.g. funcs_140.c
spans rombases 0x72B850, 0x72E260, 0x72F890), so file != overlay.

## Confidence labels

- CONFIRMED: histogram table; pair-only cluster result; pair rombase identity; helper
  callee sets and their swc1 contents; store-base provenance chains quoted above; absence
  of any base-exe callee writing offsets 0x0..0x14.
- INFERRED: rombase = per-overlay image identity (assumes overlays load at 0x08000000,
  consistent with all observed vram/ROM deltas); sp-heavy writers being non-camera;
  "two camera modes in one overlay" interpretation of the pair; area weights.
