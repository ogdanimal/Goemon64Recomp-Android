# Goemon64 — Default (ordinary-area) camera writer: final report

Scope: routes (a) template/objA, (b) CamTween callers + vram gap, (c) integer-copy scans — all
pursued to completion. New evidence files in this scratchpad: `api_func_8000F420.c`,
`api_func_8000F6E8.c`, `api_func_8000FE1C.c`, `api_func_80035854.c`, `api_func_800358E8.c`,
`api_func_800357BC.c`, `api_func_80035964.c`, `api_func_80011CAC.c`, `api_func_801D25EC.c`,
`api_func_8001C3E0.c`, `api_func_8001B950.c`, `api_func_800196F0.c`, `api_func_8001E380.c`,
`api_func_801D23C0.c`, `api_func_801F8EDC.c`, `api_func_801F8F0C.c`, `wr_801F8FD0.c`,
`upd_8020D998.c`, `task_801CB800.c`, `task_801D98E0.c`, `cam_801E8EC4.c`,
`scan_mask_writers.py`, `scan_2c_writers.py`, `scan_node2c_stores.py`, `romscan.py`.

Legend: [V] = verified at instruction level this pass; [I] = inferred.

---

## 0. Memory map correction (important context for everything below)

From `RecompiledFuncs/recomp_overlays.inl` section_table [V]:

| slot | vram | files (rom) |
|---|---|---|
| main exe | 0x80000450 (rom 0x1050, size 0x7DBD0) | always resident |
| game-mode slot | 0x801CB460 | file_11 (0x587370, **ordinary gameplay/player module**), file_13 (0x5F6840), file_14 (0x63A2B0), file_15/16/20/22/23 — one at a time |
| companion slot | 0x8020D2A0 | file_12 (0x5C8770, 0x2E0D0 — gameplay companion), file_17 (0x66C250), **file_18 (0x676C80 — CamTween camera-sequence module)**, file_19 (0x694CA0), file_21 (0x69D4E0) |
| scripted areas | 0x08000000 | files 24..80 (relocatable, funcs_100–143) |

Consequence: several prior "base-exe" functions at vram 0x8020D2A0+ (e.g. func_8020D998_677378,
func_8021F9CC_6893AC) are NOT always-resident; they belong to companion overlay file_18.
Same-vram functions in different funcs_*.c files are different overlays (e.g. the
`jal 0x80012940` at 0x801CB738 in funcs_99.c belongs to tiny file_20 (rom 0x69CBD0), NOT to
file_11's player spawn — file_11's 0x801CB738 is a nop delay slot) [V].

## 1. The default Camera struct is 0x8020CBF0 — and 0x8020CBF0 is NOT a spawn template

The prior "template 0x8020CBF0 with function-pointer slots" theory is dead. func_801D23D4_58E2E4
(camera init, funcs_32.c; called from player spawn at 0x801CB734 [V]) does:

- `0x801D23DC lui s0,0x8021 / addiu s0,-0x3410` → s0 = **0x8020CBF0**; `0x801D23EC jal 0x801D25EC`
  with a0=s0 → fills it as a 0x60-byte **Camera image** (see §2).
- `0x801D2438..0x801D2458` integer copy loop: 0x60 bytes 0x8020CBF0 → 0x8020CC50 (second camera).
- `0x801D245C lui at,0x2000 / addu a1,s0,at` → a1 = **0xA020CBF0 (uncached alias!)**;
  `0x801D24A4 jal 0x80035854` with a0=0x801D23C8 (the proven no-op callback), a1=0xA020CBF0,
  a2=0x8006D920 (main-exe DATA, not code — no func_8006Dxxx exists [V]), a3=fov float.
- v0 (objA) → 0x801FC624; `lw t0,0x18(v0)` → **0x801FC628 = objA's node** [V] (matches known fact).
- Spawn chain [V]: func_80035854 → func_800358E8 → func_800357BC → func_80035964, where at
  **0x80035988: `sw a1, 0x2C(node)`** (node = *(obj+0x1C)) — the camera pointer is stored in the
  node's +0x2C slot **as the uncached address 0xA020CBF0**, and `0x8003598C sw a2,0x30(node)`
  stores the 0x8006D920 config. THE 0x8FFFFFFE MASK MYSTERY IS SOLVED: 0xA020CBF0 & 0x8FFFFFFE =
  0x8020CBF0 — the mask converts the stored uncached pointer to a cached CPU address [V].

So: **Camera pointer chain *(0x801FC628)+0x2C & 0x8FFFFFFE = 0x8020CBF0** during ordinary gameplay.
eye Vec3f at 0x8020CBF0, look_at at 0x8020CBFC. 0x8020CBF0 lies ABOVE file_11's ROM image end
(0x801CB460+0x41400 = 0x8020C860) → it is file_11 **BSS** [V]; that is why it never appears as a
ROM data word.

Cross-corroboration [V]: scripted-area overlays build a1 = 0x8020CBF0 (lui 0x8021/addiu −0x3410,
dozens of sites in funcs_100–136) and pass it to func_8000F420 — which chains to func_8000F6E8 →
func_8000FE1C, a **positional-audio pan/volume routine** that reads eye (+0/4/8) and at (+0xC) and
ends in audio call func_80038C30. Overlays treat 0x8020CBF0 as "the camera" for sound
spatialization → it IS the active gameplay camera.

Related engine globals (main-exe BSS, all [V]):
- 0x801684A0 / 0x801684F0 — **CORRECTED 2026-07-19: these are NOT camera globals.**
  They were previously described here as "active camera NODE pointer" and "active
  Camera struct pointer, cached/masked". They are actually per-object, per-frame
  animation/skeleton-walker scratch, reassigned for EVERY drawn object every frame
  by func_80016C44. This repo's own patches/anime.c already decompiles them that
  way: anime.c:50 assigns `D_801684A0_1690A0 = object` inside the render walker,
  and anime.c:108-122 holds an `AnimatedSkeleton*`/animation pointer in
  D_801684F0_1690F0. func_8001C3E0 does write both, but its own list-walk loop
  clobbers 0x801684F0 again before the function returns (@0x8001C530, @0x8001C5D0),
  and func_8001B950 writes them too (@0x8001B9AC, @0x8001B9D8) — shared graphics
  scratch, not camera state.
  CONSEQUENCE: they are useless as a camera-cut / area-change signal, both because
  they are not camera state and because func_8001C3E0's only gameplay caller
  (func_801D98E0_5957F0) derives BOTH its arguments from fixed globals, so the
  value would not change across a default-camera → default-camera transition
  anyway. For area transitions use the map id at 0x800C7AB2 instead (see below).
- 0x801684B0 = per-frame camera matrix built from the active NODE's transform (+0x8..+0x24
  pos/rot/scale) by func_8001E380 (@0x80017214) and fed to v9's patched view builder
  func_80017D8C (@0x80017224, a1=0x801684B0) [V].
- 0x80168510 = fov, 0x80168518 = derived float, *(0x8015C5C8)+0x3AE1C = engine camera slot
  (written by file_18 code only).

## 2. Initial pose writer (room-entry default)

func_801D25EC_58E4FC [V]: selects by mode byte *(0x800C7AA4): mode 0/3 → template 0x8020C7E8,
mode 2 → 0x8020C848, else → 0x8020C8A8 (all in file_11 DATA, rom-visible), and integer-copies
0x60 bytes into the Camera (a0=0x8020CBF0). Fov defaults 32.5/40.0 range set afterwards in
func_801D23D4 (0x4202/0x4220 constants).

## 3. Who writes eye/at per frame in ordinary areas — ANSWER: NOBODY (event-driven design)

Exhaustive negative, four independent scans over all 148 funcs_*.c [V]:
1. Direct lui/lo16 loads or stores to any field 0x8020CBF0..0x8020CC10: **zero** (all access is
   pointer-based).
2. All 180 `& 0x8FFFFFFE` masked camera-pointer reads, alias-tracked forward for stores to
   +0x0..+0x14 (`scan_mask_writers.py`): only THREE writers in the entire binary:
   - **func_801F8FD0_5B4EE0** (file_11) — full eye+at writer, see §4. UNREFERENCED.
   - func_08000000_71A510 (area overlay, at.x only) — scripted-area code.
   - func_80016C44_17844 — false positive (store is to the display-list head 0x8016C5CC, the
     masked camera is passed read-only to func_800196F0) [V].
3. All unmasked `lw rX,(rY+0x2C)` loads followed by ≥2 stores to +0x0..+0x14
   (`scan_2c_writers.py`): every hit outside §2's list is either a node-position write
   (+0x8/C/10 = node pos, not camera) or file_18/19/area-overlay cinematic code.
4. Integer copy-loop commits onto a camera: only the func_80012900/func_80012878 pair exists;
   func_80012900 has exactly TWO jal callers in the whole binary (see §5); no other 0xC-stride
   lw/sw loop targets a camera-aliasable destination (§3.1-3 cover pointer provenance).

And the per-frame task that IS registered at player-spawn end — func_8003521C(0x801CB800)
[V @0x801CB784] → func_801CB800 → func_801D98E0_5957F0 — does NOT write the pose. It is the
**camera command processor** [V]: consumes command byte objA+0xE0, indexes per-mode list table
0x801FCC5C (file_11 data), calls func_8001C3E0(*(0x801FC60C), *(node+0x2C), lists) =
"set active camera + viewport list" (masks and publishes 0x801684A0/0x801684F0, resolves
segmented per-area camera pointers from the mode lists), syncs enable flags of the two camera
sub-objects (0x801FC62C/0x801FC634 +0x64). Command byte objA+0xE0 writers: area overlays only
(funcs_119/120/123/134) plus the file_11 sites in the 0x801D92xx variant [V].

The object created right after camera init (updater func_801E8EC4_5A4DD4, handle at 0x801FC638,
+0x5C=player) is a list-expiry/GC walker, not camera [V]. The objA object callback 0x801D23C8 is
the known no-op [V, prior]. **There is no hidden per-frame updater in the "template": route (a)'s
premise dissolves — 0x8020CBF0 is the camera itself, and the camera object needs no update
callback because nothing recomputes its pose per frame.**

Conclusion: Goemon64's ordinary-area camera is a **fixed-pose / cut-based system**. The pose in
0x8020CBF0 changes only on events:
- scripted-area overlay writers (the ~6+10 known inline writers) write absolute poses through
  *(0x801FC628)+0x2C — i.e. INTO 0x8020CBF0 — on zone triggers;
- camera commands (objA+0xE0) switch active camera/viewport lists per zone;
- CamTween sequences ease the pose (§5).
This matches the movement resolver's camera-cut hysteresis (blend/snap logic on *(objA)+0xAB),
which is precisely held-stick-across-cut handling for a cut-based camera — and explains why five
prior swc1-histogram hunts found "no default writer": there is none to find.

## 4. func_801F8FD0_5B4EE0 — the (dead) player-follow snapper

Full decode [V] (`wr_801F8FD0.c`): gets objA via func_801D23C0 (= `return *(player+0x64)` [V]),
camera = *(objA→0x18→0x2C) & 0x8FFFFFFE, then:
- at.x (+0xC) = playernode->pos.x (@0x801F914C); at.y (+0x10) = pos.y + 20.0 (@0x801F9174);
  at.z (+0x14) = pos.z (@0x801F9180); eye.y (+0x4) = pos.y + 21.0 (@0x801F9190);
- offset presets by mode (engine +0x3ADE4/+0x3ADFA bytes or override angle/dist at
  0x800C7AEC/0x800C7AF0): e.g. (100.0, −100.0), (−40.0, 0) …;
- func_80033898(0, heading=*(player+0x94), …) rotates the preset by PLAYER HEADING;
- eye.x (+0x0) = pos.x + dz (@0x801F91C8); eye.z (+0x8) = pos.z + dx (@0x801F91DC).

It recomputes eye purely from player position + heading — **no feedback** from the previous
camera. HOWEVER it is **unreachable in this build** [V]: no `jal`, no computed lui/addiu/ori
build (−0X7030 / 0X8FD0 greps empty), no big-endian 0x801F8FD0 word anywhere in mnsg.z64
(`romscan.py`), no reloc targets section 2 at offset 0x2DB70 (zero relocs target section 2 at
all), no fallthrough from func_801F8F0C (ends `jr ra` @0x801F8FC8) [V]. It is dead/leftover code
— but it documents exactly what a "behind-the-player follow cam" looks like in this engine, and
its neighbors (func_801F8EDC/801F8F0C, camera-mode bookkeeping) ARE live.

## 5. The CamTween system (route (b) — fully mapped)

- **func_80011CAC_128AC = the per-frame tween updater** (main exe; the function preceding the
  0x800126E0 gap). Steps channel state at handle+0x5C (+0x18/20/24/28/2C/30/40/48/50/58), then
  [V, @0x80012164-0x800121B4] writes image = *(handle+0x6C): eye.x/y/z ← chan+0x24/28/2C at
  image+0x0/4/8, at.x/y/z ← chan+0x30/34/38 at image+0xC/10/14, fov halfword at image+0x1A;
  then an additive shake pass (gated by byte chan+0x1) onto image +0x0..+0x14. Registered as the
  handle's object updater by func_80012940 (`func_80034E08(a0, a1=0x80011CAC, 0)` [V]).
- func_80012940 = tween-camera constructor [V]: allocates 0x60 image from *(0x8015C5C8)+0xC7FA4,
  copies ROM default camera 0x80063740 into it, creates a node via func_80035EEC and stores
  **image|0x20000000 into node+0x2C** (@0x80012A74-0x80012A78) — same uncached convention.
  For tween cameras the handle's image IS the render camera (no commit needed).
- func_80012878 = adopt + retarget [V]: copy-loop **camera → image** (reverse direction,
  @0x80012894-0x800128B4), then re-reads the camera's eye (a1..a3 = cam+0x0/4/8 …the s0 block
  is the camera itself) into func_800127B8 (set eye channels) and cam+0xC/10/14 into
  func_80012818 (set at channels: also zeroes velocities +0xB4/BC/C4), fov via func_80012500.
  This is the FEEDBACK path: tweens seed from the camera's previous contents, then ease.
  **CORRECTION 2026-07-19: "func_80012878 has no jal callers — reached only via computed/overlay
  paths, or unused" was a SCAN ARTIFACT, and the "or unused" reading is flatly wrong.** Overlays
  never reach base-exe code by `jal`; they use `lui/addiu/jalr`. Recounted that way,
  func_80012878 has **22 indirect call sites across 13 area overlays** (funcs_133/135/137/138/
  140/105/109/129/130/131/139/141/142) — the ordinary zone-trigger camera code. The idiom is
  `lw $t6,-0x39D8($v0)` (=*(0x801FC628)) / `lw $t7,0x2C($t6)` / `and $t8,$t7,0x8FFFFFFE` →
  0x8020CBF0 passed straight in as a1 (purest form: funcs_140.c:10498-10500).
  So the feedback path is LIVE and seeds from the real default camera.
  NOTE the third channel seeded here is **fov via cam+0x1A**, not roll — see the +0x18 vs +0x1A
  entry in the standing warnings of docs/re-notes/README.md.
- Gap functions 0x800126E0-0x80012818 = channel setters/retargeters (e.g. func_800126E0 writes
  chan+0x58/+0x60 step values from spans/durations) — helpers, not drivers [V].
- **func_80012900 commit (image → camera, 0x60-byte copy loop) — "exactly two callers" is WRONG.**
  CORRECTED 2026-07-19: same jal-only scan artifact as func_80012878 above. Recounted including
  `lui/addiu/jalr`, func_80012900 has **63 indirect sites** (62 confirmed followed by a `jalr`).
  Related recounts: func_80012940 = 24, func_800127B8 = 78, func_80012818 = 98. The two `jal`
  callers below are still real and still the most important ones, but they are not the whole set:
  1. 0x8020DDEC inside **func_8020D998_677378** (file_18; a 34-state jump-table camera-sequence
     machine, table 0x8022A6C8, driving handle at obj+0x8; commits to file_18 BSS scratch camera
     0x8022B2C0 and repoints the engine camera slot *(0x8015C5C8)+0x3AE1C to it
     [V @0x8020DDE4-0x8020DE10]). Its object is created by func_80211D7C_67B75C
     (`func_80034E08(a1=0x8020D998)` [V]) — cinematic/map sequences, and it has essentially no
     player-position references (file_18 cluster: 1 ref total) — NOT a follow cam.
  2. 0x80223B38 inside func_8021F9CC_6893AC (file_18 giant script): a1 =
     *( *(0x801FC628)+0x2C ) & 0x8FFFFFFE — **commits the tween image onto the DEFAULT camera
     0x8020CBF0** [V @0x80223B14-0x80223B3C]. This is how cinematic easing lands in the ordinary
     camera when file_18 is resident.

## 6. Deliverable answers

**(1) The default-camera "writer":** There is **no per-frame default-camera writer** — CONFIRMED
by four exhaustive scan families (§3). The Camera struct 0x8020CBF0 [V-identified this pass] is
written only by: init func_801D25EC (template copy) [V]; scripted-area overlay inline writers
(known, absolute poses, via *(0x801FC628)+0x2C) [V-mechanism]; the CamTween commit
func_80012900 from file_18 call sites [V]; and (dead) func_801F8FD0 [V]. Per-frame "camera
logic" in ordinary areas is only the command processor func_801D98E0 → func_8001C3E0
(active-camera/viewport switching, no pose writes) [V].

**(2) Feedback vs recompute:**
- The live event writers (area overlays) write ABSOLUTE poses — no read-back (prior verified).
- The CamTween path is FEEDBACK-SEEDED: func_80012878 copies the camera's current 0x60 bytes into
  the tween image and loads cam+0x0/4/8 and +0xC/10/14 as the channels' starting values
  (`lw a1,0x0(s0) / lw a2,0x4(s0) / lw a3,0x8(s0)` @0x800128B8-C0 → func_800127B8; likewise
  +0xC/10/14 → func_80012818), then func_80011CAC eases and (via func_80012900) writes back —
  classic ease-from-current-pose feedback [V].
- The dead snapper func_801F8FD0 recomputes from player pos+heading, zero feedback [V].
- Between events, the struct is simply NOT touched — the view can still move via the camera
  NODE's transform (func_8001E380 builds the view matrix at 0x801684B0 from node +0x8..+0x24
  each frame [V]); any node-side motion composes into func_80017D8C without touching eye/at.

**(3) Would patching "the writer" alone rotate ordinary-area cameras?** NO single writer exists
to patch — the Zelda64Recomp-style single-point win is CONFIRMED ABSENT for this game. The
correct patch surface is the CONSUMER side, which this hunt has now pinned precisely in the
always-resident main exe:
- func_8001C3E0_1CFE0 — the single choke point through which EVERY camera (default, overlay,
  tween, per-mode list cameras) becomes active (writes 0x801684A0/0x801684F0);
- the per-frame render consumption: func_80016C44 walker → func_800196F0(camera) view build +
  func_8001E380(0x801684B0, node) node-matrix build → func_80017D8C (v9's existing hook).
A rotation applied each frame at func_800196F0-entry (rotating the eye−at pair it reads), or
kept at func_80017D8C as in v9, plus the movement-basis rotation inside func_801CE4D0 (B1 plan),
is the complete coherent solution; storage-side rotation of 0x8020CBF0 would be silently
overwritten at every camera cut/commit and must NOT be the mechanism. INFERRED (design-level):
because ordinary-area poses are cut-based absolutes, a render+basis rotation is not merely a
workaround — it is the only architecture that survives cuts.

**STRENGTHENED 2026-07-19 — the real reason storage-side rotation is forbidden.** The
"silently overwritten" argument above is the WEAKER one, and being overwritten is not the
worst outcome. The decisive hazard is that the rotated struct is READ AS A BLEND SOURCE
*before* it is overwritten: func_80012878 seeds a camera tween's `current` channels directly
from 0x8020CBF0 (eye +0x0/4/8, at +0xC/10/14, fov +0x1A), from 22 sites across 13 area
overlays. So the failure mode is not "our rotation gets lost" but "every scripted camera
move in those overlays eases from the player's arbitrary orbit pose instead of the authored
one", wrong by a player-controlled amount.
This was investigated properly on 2026-07-19 when the private-copy architecture was
re-litigated, and the copy approach was CONFIRMED correct. Two findings closed it:
  (a) there is NO hook point simultaneously downstream of all camera producers and upstream
      of all consumers — producers and the audio/projector/skybox-scroll consumers are
      interleaved inside the same object walk (func_80034734), ordered by a runtime priority
      halfword at obj+0x20. func_800012FC_1EFC is upstream of all consumers but ALSO upstream
      of all producers, and runs 2-3 times per frame; func_80016950_17550 is post-producer but
      already downstream of audio/projector/skybox-scroll (one-frame lag).
  (b) the blend-from-live path above.
DO NOT re-open this. The consequence is that each consumer needs its own hook — see the
consumer list and which are handled in patches/camera.c.

## 7. Corrections to prior KNOWN FACTS

- "template 0x8020CBF0 … learn the template layout and which slots are function pointers" →
  WRONG: 0x8020CBF0 is the Camera struct itself (file_11 BSS), passed uncached (0xA020CBF0).
- "& 0x8FFFFFFE mask" → uncached→cached pointer normalization, nothing else.
- "camera tween module 0x80012304..0x80012940" → the per-frame updater is func_80011CAC
  (0x80011CAC..0x8001230x), BEFORE the previously-scanned range; 0x800126E0-gap functions are
  channel setters.
- func_8020D998/func_8021F9CC etc. are overlay file_18 code, not base-exe; "base-exe funcs at
  0x8020D2A0+" in earlier reports should be re-attributed.
- funcs_99.c's CamTween usage at 0x801CB4xx/0x801CB73x belongs to file_20 (rom 0x69CBD0), a tiny
  standalone mode — not the file_11 player spawn.
