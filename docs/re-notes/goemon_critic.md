# Goemon64 camera hunt — completeness-critic report (cross-check of 5 RE agents)

Method: read all five reports in full; identified every load-bearing single-source claim; re-verified the
critical ones directly against the recompiled C (extractions + exhaustive greps, both `MEM_W` argument
orders). New evidence files in this scratchpad: `chk_801CB5D0.c`, `chk_801CE3F0.c`, `chk_801CE4D0.c`,
`chk_801CEAD4.c`, `chk_801D23C8.c`, `chk_greps.txt`, `chk2_greps.txt`.

Legend: **[V]** = verified by me in this pass (instruction-level); **[C]** = confirmed by the reporting
agent with quoted disasm and consistent across reports; **[I]** = inferred.

---

## VERDICTS on the two goal questions

### [A] Where is the player world position — **ANSWERED**

> **pos_node = `*(u32*)0x801FC60C`; x/y/z = f32 at `+0x8 / +0xC / +0x10`.
> Equivalently `*( *(u32*)0x801FC604 + 0x18 ) + 0x8/0xC/0x10` (0x801FC60C is a boot-spawn-time cache of
> player->0x18). Heading binang = `*(u16*)(0x8020C860 + 0xA4)` = 0x8020C904.**

Evidence chain, now fully verified [V]:
- Spawn site `func_801CB5D0_5874E0` (funcs_26.c): `0x801CB668 jal 0x800358E8` (spawn → v0);
  `0x801CB680 sw $v0,0x0($s0)` with s0 = 0x801FC604 (built `lui 0x8020` / `addiu -0x39FC`);
  `0x801CB684 sw $v1,0x5C($v0)` with v1 = 0x8020C860. Then — decisive for provenance — the code
  **reloads the pointer from the global**: `0x801CB6A0 lw $a0,0x0($s0)` before
  `0x801CB6AC lw $t1,0x18($a0)` → `0x801CB6B8 sw $t1,-0x39F4($at)`. So
  `*(0x801FC60C) = *( *(0x801FC604) + 0x18 )` with zero register-aliasing ambiguity.
- Exhaustive writer check [V]: the only stores to 0x801FC60C in all 148 files are funcs_26.c:290/294
  (the one `sw` + its branch-duplicated delay-slot copy); no store in the reversed `MEM_W(reg,-0X39F4)=`
  form exists either. Same result for 0x801FC604 (computed-address store in func_801CB5D0 only).
- Independent convergence: player-pos's integrator/displacement trace (func_801CC4C0 pos+=vel on the
  node chain; func_801CD084 new−old diff on exactly these offsets) [C] and trace-target's structural
  discovery (`*(obj+0x18)` → pos +0x8/C/10, rot u16 +0x14/16/18, from func_802171A8) [C] were produced
  from disjoint functions and agree.
- Identity as "the player": the object is the one steered by the device-validated movement resolver
  (func_801CE3F0/801CE4D0 operate on it via +0x5C/+0x64/+0x90), receives stick-driven steering
  (func_801CEAD4) and velocity integration. Residual formality: a one-off live dump of
  `*(0x801FC60C)+0x8/C/10` against on-screen position is the final rubber stamp, but no static evidence
  points anywhere else.
- **Invalidated check** from trace-target: "dump `*( *(0x8020CA2C)+0x18 )+0x8/C/10`" is bogus — the
  record at `*(0x8020CA2C)` is a 0x18-byte static table entry (see C2 below); +0x18 reads past its end
  into the next record. Do not spend device time on it.

### [B] Minimal patch point rotating the game's OWN camera yaw so view AND movement rotate together — **PARTIAL**

Split into the two readings of the question:

**B1 — coherent view+movement rotation (render-style, the practical v9 upgrade): answered, with one
correction.** The verified minimal set is exactly **two hooks**:
1. `func_80017D8C_1898C` (view builder; exactly two jal callers, funcs_2.c 0x80016AE8 and funcs_19.c
   0x80017224 [V]) — v9's existing patch point; rotate the view by θ here.
2. `func_801CE4D0_58A3E0` (sole pose→movement-basis converter) — rotate **only the Camera eye−look_at
   delta** (built at 0x801CE514-0x801CE544 into sp+0x4C/50/54, before the normalize call at
   0x801CE558) by the same θ.
   Chain topology verified [V]: func_801CE4D0 has exactly ONE caller (`jal` at 0x801CE4B0 inside
   func_801CE3F0; the only computed `-0x1B30` build in the binary, funcs_66.c 0x801FE4B4, is
   `lui 0x8020`+`-0x1B30` = 0x801FE4D0 — a different function, false alarm). func_801CE3F0 has exactly
   ONE caller function, func_801CD310_589220, with two call sites (0x801CD394 and 0x801CDE90; the
   second temporarily swaps player+0x84/88/8C to (0,1,0) around the call). A hook inside CE4D0 covers
   all paths.
   **Correction to global-camstate's prescription**: do NOT also rotate the `*(0x8020CA2C)` record
   (+0x10/+0x14). Per the usage evidence (C2 below) the record is the player's processed STICK input,
   not a camera direction; rotating it would rotate player input on top of the rotated basis
   (double-rotation → view/movement mismatch again). The eye−at delta is the only camera-derived input
   to the resolver.
   Residual inference in B1: the exact stick×basis composition inside func_801CE870 (+ helpers
   8001D460/8001D898/8001D718) is data-flow-inferred, not instruction-verified — reading it (or a live
   test) is the remaining confirmation that rotating the basis rotates movement 1:1.

**B2 — true Zelda64Recomp-style orbit of the game's own camera (input side): UNANSWERED.** Verified
negatives: no shared base-exe eye/at write function serves the overlay camera routines (all 23 callees
of the 0x72E260 pair checked, eye/at fully inlined per overlay [C]); ~6 substantial + ~10 partial
distinct overlay writers across ~12 overlays [C]; and the DEFAULT (ordinary-area) camera writer was
never identified by any agent. New findings from this pass:
- The "camera controller callback 0x801D23C8" (passed with template 0x8020CBF0 at objA spawn,
  global-camstate §4) is a **2-instruction no-op stub** (`sw a0,0x0(sp); jr ra; sw a1,0x4(sp)`) [V] —
  dead end; the real per-frame update is reached some other way (template 0x8020CBF0's other members,
  or a separately registered updater à la `func_8003521C(0x801CB800)` seen at player-spawn end).
- Methodology gap in overlay-scan: the swc1 histogram **cannot see memcpy-style Camera commits**
  (integer `lw/sw` copy loops — exactly how func_80012900 writes eye.x at 0x80012918). A base-exe
  default camera committed by such a loop would score zero. The negative "no base-exe writer found" is
  therefore incomplete, and the CamTween-updater gap (0x800126E0-0x80012810, math-helpers' open
  question) plus callers of func_80012900/func_80012878 are the right places to look next.

---

## Contradictions between reports (winner + evidence)

| # | Topic | Positions | Resolution |
|---|---|---|---|
| C1 | Writer of 0x801FC604 | trace-target: "no sw to −0x39FC anywhere; writer not pinned; player-vs-NPC open" vs player-pos/global-camstate: func_801CB5D0 @0x801CB680 | **player-pos wins [V]**. trace-target grepped only direct-offset stores; the writer uses `addiu s0,-0x39FC` + `sw v0,0x0(s0)`. Its open question "identity of *(0x801FC604)" is resolved by the spawn/steering context. |
| C2 | Nature of `*(0x8020CA2C)` / the 0x800C7DB0 records | KNOWN FACTS: "movement-state heap object"; global-camstate: "camera-direction record {len, planar dir}"; player-pos: "movement-parameter table (speeds/turn rates)" | **Neither label survives; corrected synthesis: per-context processed ANALOG-STICK record** (halfwords +0x0..0xB = button/state fields incl. +0x0 bit0 override; +0xC = stick magnitude; +0x10/+0x14 = planar stick components). Evidence [V]: func_801CE3F0 dead-zone gate (`rec+0xC==0.0` → clear camera-cut flag & bail, 0x801CE438-0x801CE474) and unit-dir build `(-rec10/rec0xC, rec14/rec0xC)` (0x801CE47C-0x801CE494); func_801CE4D0 gate `rec+0xC < 0.3` (deadzone-sized threshold, 0x801CE600-0x801CE614) and post-camera-cut hysteresis that BLENDS while record yaw is steady but SNAPS when it jumps ≥ ~4° (0x801CE61C-0x801CE638) — classic held-stick-across-camera-cut logic, meaningless for a camera-direction record; func_801CEAD4 steering step = `trunc(rec[0x10]×96.0)` into +0xA8 (clamp ±0x2D0) and pitch target = `rec[0x14]×const` (clamp ±0x3800) (0x801CEAF0-0x801CEC6C) — proportional stick steering/pitch, absurd for camera-dir components. Mechanics all three agents reported (table base 0x800C7DB0+24×idx, single writer func_801CC4C0 @0x801CC79C) are correct and re-verified exhaustively [V]. |
| C3 | Identity of 0x801FC614/61C | trace-target: "engine camera-state blocks / default-camera pose pair" (basis of its next-hunt recommendation) vs player-pos: "companion objects" / global-camstate: "player-subsystem objects" | **player-pos/global-camstate win [V]**: they are `func_80035EEC(player,2,1)` sub-node results (0x801CB6B4/0x801CB6CC), each linked to the player's position node via `func_8001C9BC(part, *(0x801FC60C))` (0x801CB6E8/0x801CB6FC) — player-rig parts, not camera state. trace-target's recommendation to hunt the follow camera in 0x801CBxxx-0x801CCxxx is misdirected: that cluster is the player/movement system; the camera-controller init is func_801D23D4 (0x801D2xxx), invoked FROM player spawn at 0x801CB734. |
| C4 | func_801DACDC semantics | math-helpers: actor set-animation wrapper (full extraction) vs overlay-scan: "sound-effect-like" vs player-pos: "camera-follow helper" | **math-helpers wins** (anim-table indexing, rate/100, segmented anim ptr — quoted disasm). The cutscene sets player animation 0x55 / resets 0; it does not "follow". Mislabels don't affect either verdict. |
| C5 | `*(0x8015CD60)`+0x8/C/10 trio | trace-target: world-space point (epicenter; fed as x,y,z to look-at setters + debris placement) vs global-camstate: {angle-deg 60..90, distance 434, param 5} scalars (range checks in overlay func_0800221C, rombase 0x6BF750) | **Both have direct evidence in different overlays** → the block is per-area-repurposed scratch/params; the v9-era "angle/distance" reading is not universal. Not load-bearing: only 3 readers of 0x8015CD60 exist in the whole binary (triple-agreed [C]) and the movement resolver is not among them, so it is the wrong lever for [B] regardless. |
| C6 | Line-124 callee 0x80031790 | math-helpers/trace-target: calltargets.py double-applied a branch-duplicated delay-slot `addiu -0x7438`; real target func_80038BC8 | **Confirmed [V]** in target_func_0800037C.c lines 111-124: the delay slot appears on both branch paths; 0x80040000−2×0x7438 = 0x80031790. KNOWN-FACTS entry is wrong; any other calltargets.py-derived target deserves a spot-check. |
| C7 | Minor | player-pos prose "eye (1826,167,27)" vs trace-target hex 0x44E40000 = 1824.0f; store-address ranges differ slightly | trace-target's hex is decisive; typo-level, no impact. |

Non-contradictions worth noting: "nothing touches 0x8020CA2C" (math-helpers) is scoped to its helper
set and consistent with the writer found elsewhere; the "0x8020C6xx → 0x801FC6xx" +0x10000 correction
(global-camstate) is implicitly confirmed by every verified lui/lo16 pair in this pass.

## Spot-check results (claims re-verified this pass)

1. Spawn provenance chain in func_801CB5D0 — **holds** (see [A]).
2. Single-writer status of 0x801FC60C, 0x801FC604, 0x8020CA2C — **holds**, both MEM_W argument orders
   checked; only the known sites exist.
3. func_801CE4D0 body vs global-camstate §2.2 — **instruction-accurate**: chain
   `lw a2,0x64(a0)` → `lw a3,0x18(a2)` → `lw v0,0x2C(a3)` → `and 0x8FFFFFFE` (0x801CE4EC-0x801CE510),
   eye−at subtraction (0x801CE514-0x801CE544), atan2 of rec+0x10/+0x14 (0x801CE548-0x801CE554), basis
   stores to ctl+0x6C/yaw +0x6A (0x801CE644-0x801CE660), 0x4F blend counter on `*(0x801FC624)+0xAB`
   (0x801CE578-0x801CE58C), 0.3 double at 0x8020A970 (`lui 0x8021`−0x5690).
4. Caller-chain uniqueness of CE4D0/CE3F0 — **holds** (one jal each; second CE3F0 site is inside the
   same caller func_801CD310; the only computed −0x1B30 build resolves to 0x801FE4D0, not 0x801CE4D0).
5. func_80017D8C caller count — **holds** (2 jal sites).
6. Sibling func_08000B54_72EDB4 exists in funcs_140.c — **holds**.
7. calltargets.py double-addiu bug — **reproduced** (C6).
8. func_801D23C8 — **no-op stub** (new; kills the "callback = default camera updater" hypothesis).

## Gaps worth one more targeted look (prioritized)

1. **func_801CE870_58A780 + helpers 8001D460/8001D898/8001D718, instruction-level** — the one remaining
   inference in the B1 plan (exact stick×basis composition). Confirms that rotating the eye−at delta
   alone rotates movement 1:1, and settles whether the record components are controller-space or
   screen-space.
2. **Ordinary-area (default) camera writer** — three concrete routes: (a) parse template 0x8020CBF0 in
   the data image for function pointers (objA spawn arg; its callback slot held the no-op 0x801D23C8,
   so the update pointer is elsewhere in the template or registered separately); (b) enumerate callers
   of func_80012900/func_80012878 and read the CamTween-updater gap 0x800126E0-0x80012810 — a
   memcpy-committed default camera would explain why the swc1 histogram missed it; (c) re-scan base-exe
   for integer copy loops targeting a pointer that aliases `*(0x801FC628)+0x2C`.
3. **Record producer** — who fills 0x800C7DB0 records / the 0x8023B3A0 BSS working set
   (candidates already named: func_801DECE0_59ABF0, func_801DF3D8_59B2E8, func_801CF770_58B680,
   func_80222E4C_5DE31C). Directly confirms/refutes the stick-record reinterpretation (C2).
4. **Live rubber stamps** (one session): dump `*(0x801FC60C)+0x8/C/10` vs on-screen position; observe
   `*(0x8020CA2C)` record +0xC/+0x10/+0x14 while wiggling the stick (instant confirmation of C2).
   Drop the invalid `*( *(0x8020CA2C)+0x18 )` dump.
5. **Audit remaining calltargets.py-derived KNOWN-FACTS addresses** for the double-addiu bug class
   (one wrong entry already found; the tool bug is systematic on branch-duplicated delay slots).

## Bottom line

[A] is answered with writer-side static proof and triple convergence. [B] is answered for the practical
two-hook coherent-rotation design (80017D8C + 801CE4D0, rotating the eye−at basis only — one correction
to the reported recipe), but the "rotate the game's own camera" ideal has no single patch point: special
areas are per-overlay inlined writers, and the default-area camera writer is still unfound, with the
histogram-blind-spot (integer-copy commits) and the CamTween updater gap as the best remaining leads.
