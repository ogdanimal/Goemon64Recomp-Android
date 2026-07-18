# Goemon64Recomp — classification of base-exe helpers called by overlay camera func_0800037C_72E5DC

Static RE, offline. Sources: extracted recompiled C in this scratchpad (`helper_*.c`), call-site
context from `target_func_0800037C.c`. Every claim tagged CONFIRMED (direct disasm evidence quoted)
or INFERRED (plausible reading of confirmed mechanics).

## Headline results

1. **The "0x80031790 callee" does not exist — it is a calltargets.py artifact.** Both call sites at
   target lines 124 (`jalr@0x0800042C`) and 408 (`jalr@0x080005B8`) build the SAME target:
   `0x08000420: lui $t9, 0x8004` + `0x08000428: addiu $t9, $t9, -0x7438` → **0x80038BC8** (sound
   effect player). At the line-124 site, the recompiler duplicated the delay-slot `addiu` across
   the `bne` at 0x08000424 (it appears at target_func lines 116 AND 120); calltargets.py applied it
   twice: 0x80040000 − 0x7438 − 0x7438 = 0x80031790. Vram 0x80031790 itself lies mid-body of the
   huge base-exe function **func_80030730_31330** (RecompiledFuncs/funcs_17.c lines 5310–10714;
   `// 0x80031790: c.lt.d $f16, $f18` at file line 8521) and is never a call target. CONFIRMED.

2. **The 0x800124A0–0x80012900 family is a camera TWEEN CONTROLLER class** (the object at
   `*(s0+0x10)` in the target func) with 7 linearly-interpolated channels (1 scalar + two Vec3f),
   and **func_80012900 is the per-frame COMMIT that copies the controller's finished 0x60-byte
   camera image over the render Camera struct** (`*(s0+0x14)`, whose layout matches the known
   render Camera: +0x00 eye Vec3f, +0x0C look_at Vec3f, +0x1A s16). This is the strongest lead
   yet for the eye-writer hunt (see "Implications" at the end).

3. **No polar-to-cartesian (angle→position) assembly anywhere in this helper set.** None of the 20
   functions calls math_sin (0x80003E10) or computes sin/cos/atan2/sqrt. Camera points move by
   per-axis linear steps toward literal world-coordinate targets. CONFIRMED (by absence, over the
   full extracted bodies).

4. Globals touched: `0x8015CD60` is read only by the already-known func_8021927C.
   **func_80221ED4 writes the SAME parent struct** (0x8015CC30: clears bit 0 of the u32 at +0xD4 =
   0x8015CD04; the camera-params pointer 0x8015CD60 is that struct's +0x130). **Nothing in this set
   touches 0x8020CA2C.**

---

## The tween-controller object ("CamTween", a0 of the 0x8001-family helpers = `*(s0+0x10)`)

Derived field map (all offsets relative to object base; `v0 = a0+0x5C` is the channel block base
used inside the helpers):

| offset | meaning | evidence |
|---|---|---|
| +0x5C | channel block base | `addiu $v0, $a0, 0x5C` in 124AC/12518/12628/12380 |
| +0x6C | ptr → 0x60-byte OUTPUT camera image (heap) | freed by dtor 12304; source of 12900 copy |
| +0x70 | f32 shared tween duration/divisor | written by 124A0; read as `v0+0x14` divisor in 124AC/12518/12628 |
| +0x74 / +0x78 / +0x7C | scalar channel current / target / step | 124AC |
| +0x80,+0x84,+0x88 | Vec3-A current (x,y,z) | 12518 reads `v0+0x24/28/2C` |
| +0x98,+0xA8,+0xA0 | Vec3-A target (x,y,z) ← args a1,a2,a3 of 12518 | swc1 at 0x80012528/30/40 |
| +0x9C,+0xAC,+0xA4 | Vec3-A step (x,y,z) | swc1 `v0+0x40/50/48` |
| +0x8C,+0x90,+0x94 | Vec3-B current (x,y,z) | 12818 writes; 12628 reads `v0+0x30/34/38` |
| +0xB0,+0xC0,+0xB8 | Vec3-B target (x,y,z) ← args a1,a2,a3 of 12628 | swc1 at 0x80012638/40/50 |
| +0xB4,+0xC4,+0xBC | Vec3-B step (x,y,z) | swc1 `v0+0x58/68/60`; zeroed by 12818 |
| +0xC8 s16, +0xCA s16 | path-playback counter / duration | 125F0 |
| +0xCC | ptr → path-playback block {u8 active, s16 mode, f32 speed, void* data} (heap) | 125F0 writes; dtor frees |

Steps are stored as **positive magnitudes** (`neg.s $f0,$f0` applied when target < current in
124AC/12518/12628), so step==0 ⇔ channel idle — exactly what the busy-mask func 12380 tests.

Call-site proof that this drives the cutscene camera (CONFIRMED, target_func_0800037C.c):
- case 4 (L_080007C4): `80012818(obj, g+8, g+C, g+10)` (g = `*(s1+0x84)` = `*(0x8015CD60)` params);
  `800124A0(obj, 30.0f)` (0x41F00000); `800124AC(obj, 5120.0f)` (0x45A00000);
  `80012518(obj, 1823.75f(0x44E3E000), 184.0f(0x43380000), 24.0f(0x41C00000))`;
  `80012628(obj, 1725.25f(0x44D7A000), 166.0f(0x43260000), 3.0f(0x40400000))` — literal WORLD
  COORDINATES: Vec3-A ≈ camera eye target, Vec3-B ≈ look-at target (INFERRED from magnitudes and
  from the render-Camera constants written later: eye (1824,167,27) at `*(s0+0x14)+0x00`,
  look_at (overlay_float,168,8) at +0x0C).
- then polls `80012380(obj)` (line 849) and only advances state when the mask is 0; finally
  `80012304(obj)` destroys it and `*(s0+0x10)=0` (lines 867–873).
- per-frame tail (L_08000B1C): if `*(s0+0x10) != 0` → `80012900(*(s0+0x10), *(s0+0x14))` then
  `(s1+0x8A)++` — the controller output is copied onto the Camera every frame.

---

## Per-function classification

### func_80012304_12F04 (93 lines) — CamTween DESTRUCTOR (game-specific)
Frees `*(obj+0x6C)` and `*(obj+0xCC)` into the heap at `*(u32*)0x8015C5C8 + 0xC7FA4` via
func_80014B74, then calls func_80034EF8(obj) (unregister), returns 0.
Evidence: `0x8001230C: lw $t6, 0x6C($a0)`; heap addr built by `lui $a0,0x8016` /
`lw $a0,-0x3A38($a0)` (= *(0x8015C5C8)) + `lui $at,0xC; ori $at,$at,0x7FA4`; `jal 0x80014B74`
twice (0x80012330, 0x80012358); `jal 0x80034EF8` (0x80012364); `or $v0,$zero,$zero`. CONFIRMED.

### func_80012380_12F80 (212 lines) — tween BUSY-MASK query (pure read, returns bitmask)
Returns v0 = OR of bits for every channel whose step exceeds the double constant at 0x8007B188
(`lui $at,0x8008; ldc1 $f0,-0x4E78($at)`; value INFERRED ≈ 0.0):
bit 0x40 ← scalar step +0x7C (`lwc1 $f4,0x7C($a0)` at 0x80012380); bit 1 ← A.x step `v0+0x40`;
bit 2 ← A.y `v0+0x50`; bit 4 ← A.z `v0+0x48`; bit 8 ← B.x `v0+0x58`; bit 0x10 ← B.y `v0+0x68`;
bit 0x20 ← B.z `v0+0x60` (each via `c.lt.d $f0,…` + `ori`). No writes. CONFIRMED.
Target usage: "wait until camera tween finished" (lines 643–653, 849–859).

### func_800124A0_130A0 (15 lines) — setter: tween DURATION
`mtc1 $a1,$f12; swc1 $f12,0x70($a0)` — that is the whole function. Sets the shared divisor used
by 124AC/12518/12628. CONFIRMED. (Target calls with 50.0, then 5.0, then 30.0.)

### func_800124AC_130AC (81 lines) — scalar-channel tween setup (lerp-toward-target)
Stores target: `swc1 $f12,0x78($a0)`; step = |target − current(+0x74)| / duration(+0x70):
`sub.s $f0,…` (+`neg.s` when `c.lt.s $f12,$f4` i.e. target<current), `div.s $f4,$f0,$f18`
(f18 = `v0+0x14` = +0x70), `swc1 $f4,0x20($v0)` (= +0x7C). CONFIRMED.
Target: `800124AC(obj, 5120.0f)` in case 4 (scalar semantic unknown — fov/dist-like, INFERRED).

### func_80012518_13118 (185 lines) — Vec3-A tween setup (3× lerp-toward-target)
Targets: `swc1 $f12,0x98($a0)` (a1→x), `swc1 $f14,0xA8($a0)` (a2→y), `swc1 $f4,0xA0($a0)` (a3→z).
Per component: step = |target − current| / duration(+0x70) via `sub.s`/`neg.s`/`div.s`, stored to
`v0+0x40/0x50/0x48`. Currents: `0x80($a0)`, `v0+0x28`(=0x84), `v0+0x2C`(=0x88). CONFIRMED.
Called with world coords (1823.75, 184, 24) → **Vec3-A = camera EYE target** (INFERRED).

### func_800125F0_131F0 (37 lines) — start scripted PATH playback (game-specific trigger)
On block `cc = *(a0+0xCC)`: `sb 1,0x0(cc)` (activate), `sh $a1,0x2(cc)` (mode), `swc1 $f12(a3),
0x4(cc)` (speed), `sw $a2,0x8(cc)` (data ptr), `sh 0,0xC8($a0)` (counter), `sh sp[0x12],0xCA($a0)`
(duration). CONFIRMED. Target call (line 630): mode=2, speed=1.0f, data = overlay-resident
RELOC(61, +0x1360) — an overlay data blob, very plausibly a camera path/control-point list
(INFERRED; note func_80012900 copies exactly 0x60 = 8×sizeof(Vec3f) bytes, and dtor frees a
separate +0x6C buffer). Duration stack arg = 0x3C (60 frames).

### func_80012628_13228 (185 lines) — Vec3-B tween setup (3× lerp-toward-target)
Mirror of 12518 for the second vector: targets `swc1 $f12,0xB0` (a1→x), `swc1 $f14,0xC0` (a2→y),
`swc1 $f4,0xB8` (a3→z); currents `v0+0x30/34/38` (=0x8C/0x90/0x94); steps `v0+0x58/68/60`;
duration `v0+0x14` (+0x70). CONFIRMED. Called with (g+8, g+C, g+10) from the 0x8015CD60 params
(case 2) and with world coords (1725.25, 166, 3) (case 4) → **Vec3-B = LOOK-AT target** (INFERRED).

### func_80012818_13418 (33 lines) — instant Vec3-B set + tween cancel
Zeroes B steps: `swc1 $f0,0xBC/0xC4/0xB4($a0)`; sets B currents: `swc1 $f12,0x8C`, `swc1 $f14,
0x90`, `swc1 $f4(stack a3),0x94`. CONFIRMED. "Snap look-at to (a1,a2,a3), stop its motion."

### func_80012900_13500 (42 lines) — **CAMERA COMMIT COPY — flagged loudly**
Copies 0x60 bytes (8 iterations × 0xC) from `*(a0+0x6C)` to a1:
`lw $t6,0x6C($a0); addiu $t9,$t6,0x60;` loop `lw/sw` ×3 per 0xC stride until `bne $t6,$t9`.
CONFIRMED. In the target it runs EVERY FRAME with `a1 = *(s0+0x14)` = the render Camera struct
(the same struct the epilogue fills with eye=(1824,167,27)@+0x00, look_at@+0x0C, s16 0x1400@+0x1A
— matching the validated Camera layout). 0x60 bytes cover Camera +0x00..+0x5F: eye, look_at,
+0x18/+0x1A s16s, +0x1C Vec3f, +0x28 Vec3f, +0x34 f32, +0x44 vp ptr, scissor +0x48...
**So the actual per-frame eye values come from the 0x60-byte image at `*(CamTween+0x6C)`, filled
by the CamTween updater (NOT in this call list) and blasted over the Camera by this memcpy.**

### func_80014B74_15774 (130 lines) — heap FREE-with-zero (allocator, game-specific)
a0 = allocator header, a1 = block ptr. Walks doubly-linked node list from `*(a0+0x8)`
(node: +0x0 block ptr, +0x4 size, +0x8 next, +0xC prev): `beq $a1,$t6` match on `lw $t6,0x0($v0)`;
unlinks (head fixup `sw $a3,0x8($a0)` at 0x80014BB8, else `sw $a3,0x8($a2)`; back-link
`sw $a2,0xC($a3)`), then zero-fills the block: loop `sb $zero,0x0($v1)` for `*(node+0x4)` bytes.
Returns a1 (or 0 if not found). CONFIRMED. The heap used by the camera code lives at
`*(u32*)0x8015C5C8 + 0xC7FA4`; the epilogue also frees s0 itself with it (line 1210,
`or $a1,$s0` at 0x08000AB0).

### func_80038BC8_397C8 (37 lines) — PLAY SOUND EFFECT (wrapper)
`andi $t6,$a0,0xFFFF` then `jal 0x80038C30` with a1=a2=0. func_80038C30 (extracted, 176 lines)
packs `id | pan<<16 | vol<<24` and writes it into a command queue near 0x801C09C9/0x801C09FD
(`lui $t1,0x801C; addiu $t1,$t1,0x9C9; sb 0xFF,0x0($t1)` …). CONFIRMED sound-queue shape.
Target plays SFX 0x24D (line 124 site) and 0x17 (line 408 site).

### func_8003D388_3DF88 (120 lines) — scene/BGM audio-state switch (game-specific, signature level)
(a0=track-ish index, a1=scene-ish index). If `*(u32*)0x80077858` != 0: calls func_8003D428 and
func_8000C838 (no args). Always: `sw $v1,0x7900($at=0x801C0000)` → 0x801C7900 = a1;
loads obj ptr from table `0x801FC600[a1]` (`lui $v0,0x8020; … lw $v0,-0x3A00($v0)` indexed);
0x801C7904 = `lbu 0x60(obj)` or −1; then `jal 0x8003D468` with a0=`*(0x800779A0 + a0*4)`,
a1=`lh(0x80078608 + a0*2)` (0x8008<<16−0x79F8). CONFIRMED mechanics, purpose INFERRED
(music/ambience change for the camera event; called at line 684 with a0=0x1D4, a1=1).

### func_80024038_24C38 (63 lines) — SET GLOBAL FLAG BIT (game-specific)
Bit array at 0x8015C608 (`lui $t8,0x8016; addiu $t8,$t8,-0x39F8`): byte index `a0>>3`
(arithmetic, negative-safe via `addiu $at,$a0,0x7; sra`), bit `1 << (a0&7)` (`sllv`), `sb` back.
CONFIRMED. Target sets flag 0xC3 (line 696) — event-progress flag.

### func_8003F1D8_3FDD8 (35 lines) — boolean GLOBAL QUERY
`return *(u32*)0x80077858 != 0;` (`lui $t6,0x8007; lw $t6,0x7858($t6)`; returns 1/0). CONFIRMED.
Target polls it in case 3 (line 719) as a wait condition (same global gates 8003D388's first
branch — likely "audio/cutscene subsystem busy", INFERRED).

### func_80034ED4_35AD4 (31 lines) — unregister GLOBAL OBJECT (game-specific)
`lui $a0,0x8017; lw $a0,-0x254C($a0)` → calls func_80034EF8(`*(u32*)0x8016DAB4`).
func_80034EF8 (extracted) = func_80034F44(obj) + func_80035044(obj) — remove-from-two-lists shape
(INFERRED: object-system deregistration). Called once in the terminal cleanup (line 1222).

### func_801DACDC_596BEC (34 lines) — actor SET-ANIMATION wrapper (game-specific)
`andi $a1,0xFF; a2=0` → func_801DAD68(actor, anim_id, 0.0f). func_801DAD68 (extracted, 355 lines):
validates id < 0xE8 (`slti $at,$s1,0xE8`), indexes a 28-byte/entry anim table
(`lw $t7,0x3F34($t7=0x8020xxxx + actor[0x60]*4)`; entry = table + id*28), writes segmented anim
pointer `*(actor+0x18)+0x2C = entry[0] + 0x60000000` (`lui $at,0x6000; addu`), playback rate
`actor+0xD0 = (s16)entry[4] / 100.0` (double const 0x4059… = 100.0), `sb id,0xCC(actor)`, speed
f12 → `*(actor+0x18)+0x70`, then anim helpers (801DC70C/801DD498/801DAF54/8003522C/801D9C54).
CONFIRMED mechanics. Target uses it (lines 512, 978) to force an actor (player, INFERRED)
animation during the camera sequence.

### func_8021804C_5D351C (58 lines, overlay-resident base of 0x0802-segment? no — base exe hi region) — VFX/actor BATCH SPAWNER (game-specific)
`if (a1 != 0) func_802171A8(a0, 0x802139E0, 13); else func_802171A8(a0, 0x80211EF8, 8);`
(`lui $a1,0x8021; addiu $a1,$a1,0x39E0; addiu $a2,0xD` / `addiu $a1,$a1,0x1EF8; addiu $a2,0x8`).
func_802171A8 (extracted, 283 lines) reads position floats `+0x8/+0xC/+0x10` and s16 angles
`+0x14/+0x16/+0x18` from `*(a0+0x18)` and spawns via func_800358E8 with a callback
(0x8006D920|0x40000000). CONFIRMED mechanics; INFERRED purpose: spawn effect set (smoke/poof)
at the object's position when the cutscene starts. NOT camera math.

### func_80221FB0_5DD480 (26 lines) — cutscene/lock RELEASE (game-specific state clear)
`sb 0, 0x7AE2(0x800C0000)` → *(u8*)0x800C7AE2 = 0; `sb 0, -0x3A9E(0x80160000)` →
*(u8*)0x8015C562 = 0; `lw $t6,-0x39FC(0x80200000)` → p = *(u32*)0x801FC604, then
`lw $t7,0x5C($t6); sh 0,0x0($t7)` → *(u16*)(*(p+0x5C)) = 0. CONFIRMED. Called at line 1056 in the
first tear-down path right before restoring the Camera to fixed coordinates.

### func_80221ED4_5DD3A4 (22 lines) — clear CAMERA-LOCK BIT in the 0x8015CC30 global struct
`lui $v0,0x8016; addiu $v0,-0x33D0` → 0x8015CC30; `lw $t6,0xD4($v0); and $t7,$t6,-2;
sw $t7,0xD4($v0)` → **`*(u32*)0x8015CD04 &= ~1`**. CONFIRMED. Note proximity: 0x8015CD60 (the
global camera-params pointer read by func_8021927C) = 0x8015CC30 + 0x130, same struct. Bit 0 of
+0xD4 is very plausibly the "scripted camera active" flag (INFERRED). Called at line 1128 in the
second tear-down path.

### 0x80031790 — see Headline result 1. Contained in func_80030730_31330; the "call" is a
calltargets.py double-application of a branch-duplicated delay-slot `addiu`; real callee is
func_80038BC8 (sound). **Recommendation: fix calltargets.py to de-duplicate the recompiler's
delay-slot copies (track only the copy on the fall-through path, or dedupe identical
`addiu $tN` lines between lui and jalr).**

---

## Implications for the eye-writer hunt

- The cutscene camera in this overlay never computes eye = target + R(angle)·dist. It linearly
  tweens eye/look_at world points and memcpys them onto the Camera via func_80012900. Therefore
  the CamTween class has a per-frame UPDATE function (steps currents toward targets by the step
  magnitudes, runs the +0xCC path playback, and renders the result into the 0x60-byte image at
  `*(obj+0x6C)`). It is NOT called by func_0800037C (it runs from the object system) and almost
  certainly lives in the base exe between the setters and the dtor — the gap
  **0x800126E0–0x80012810 / 0x80012850–0x800128FF** (i.e. between func_80012628's end, 0x800126E0,
  and func_80012818, and between 0x80012848 and func_80012900) is the prime place to look for the
  constructor/updater pair. Whatever fills `*(obj+0x6C)` writes the EYE.
- For gameplay (non-cutscene) camera, none of this set touches 0x8020CA2C, reinforcing that the
  free-roam eye-writer is elsewhere (per-area overlays, as suspected).
- `*(s0+0x14)` is (a pointer to) the live render Camera and `*(s0+0x10)` the tween controller;
  s0 itself is heap-allocated (freed via func_80014B74 with a1=s0 at line 1210) — the whole
  cutscene-camera rig is a transient heap object.
