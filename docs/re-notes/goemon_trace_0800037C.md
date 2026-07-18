# Deep trace: func_0800037C_72E5DC — the "prime camera-writer candidate"

**Verdict up front:** this function is a **scripted earthquake-cutscene camera driver** (a one-shot set-piece), not the gameplay follow camera. It drives the render Camera exclusively through a base-exe **camera-path interpolator module (0x80012304–0x80012940)** using a heap "path handle" object, plus direct literal pose fills at the end of the sequence. **It never reads any live actor/player position** — every eye/look-at input is an immediate constant, overlay-61 read-only data, or a field of the persistent camera-state struct that this same function initialized from constants. As a by-product the trace produced a much more valuable structural discovery: **engine objects keep their world transform behind a pointer at +0x18 (pos = f32 x/y/z at +0x8/+0xC/+0x10, rotation = 3 u16 at +0x14/+0x16/+0x18)** — which is where the player position has to be read from (see §6).

Confidence legend: **[C]** = CONFIRMED by quoted disasm; **[I]** = INFERRED (plausible, stated why).

---

## 1. Entry, register roles, and the switch selector

```
0x08000390  lw   $s0, 0xD0($a0)      ; s0 = *(a0+0xD0)   — per-object STATE BLOCK (heap)
0x08000394  or   $s1, $a0, $zero     ; s1 = a0            — the overlay camera-scene object
0x08000398  lbu  $t6, 0x0($s0)       ; case index = *(u8*)(s0+0x0)
0x0800039C  sltiu $at, $t6, 0x7      ; >6 -> default (tail only)
0x080003B0  lw   $t6, 0x15E0($at)    ; jump table = overlay data +0x15E0 (RELOC seg 61)
0x080003B4  jr   $t6
```

**[C] The switch selector is `*(u8*)(*(s1+0xD0) + 0x0)`** — byte 0 of the state block s0, dispatched through a 7-entry pointer table in overlay data at +0x15E0. `a1` is stored to `0x44($sp)` at 0x0800038C and never reloaded (unused).

### s1 (overlay camera-scene object) fields used
| offset | type | role | evidence |
|---|---|---|---|
| +0x18 | ptr | (not read here, but this object type's transform block — see §6) | func_802171A8 |
| +0x84 | ptr | `g84` = persistent camera-state struct = `*(u32*)0x8015CD60` | set via call 0x080003C4 → func_8021927C (0x8021927C: `lw $t6,-0x32A0($t6)` with lui 0x8016; `sw $t6,0x84($a0)`) **[C]** |
| +0x8A | u16 | per-state frame counter; ++ at 0x08000B3C each update, zeroed on every state change | 0x08000B34-0x08000B3C **[C]** |
| +0xD0 | ptr | state block s0 | 0x08000390 **[C]** |

### s0 (state block) fields used
| offset | type | role | evidence |
|---|---|---|---|
| +0x0 | u8 | **state index (the switch selector)**, written 1/2/3/5/4/6 on transitions | 0x08000414, 0x08000690, 0x0800075C, 0x0800079C, 0x080007C0, 0x08000908 **[C]** |
| +0x4 | ptr | last spawned debris object (may be NULL) | 0x08000478, 0x08000514 **[C]** |
| +0x8 | u8 | cleared once at case-1 first frame | 0x08000434 **[C]** |
| +0x10 | ptr | **camera-path HANDLE** (base-exe interpolator object); zeroed at 0x08000880 when finished | passed as a0 to every 0x80012xxx call **[C]** |
| +0x14 | ptr | **the driven render Camera struct** (eye +0x0, at +0xC, s16 +0x18/+0x1A) | literal fills §3.4/§3.6 + a1 of func_80012900 at 0x08000B2C **[C]** (identity as the *submitted* camera: **[I]**, layout matches the validated Camera struct exactly) |

State flow: **0 → 1 → 2 → 3 → 5 → 4 → 6 (self-destruct)**. The counter==1 tests in cases 2/4/6 mean "first frame in this state" (counter is reset to 0 on transition, then the shared tail immediately increments it to 1).

---

## 2. The camera-path handle module (base exe 0x80012304–0x80012940) — fully mapped

All 0x80012xxx callees were extracted and read. They are **not trig/vec math**; they are a destination-glide interpolator that owns a heap-allocated Camera block. (No sin/cos — `math_sin` 0x80003E10 is never referenced anywhere in this function or these callees.)

### Handle object layout (offsets relative to handle = `*(s0+0x10)`)
```
+0x60/0x64/0x68   scalar-D cur/dest/step (stepped at func_80012AD8 head 0x80012AE4..)      [C]
+0x6C             ptr -> HEAP-ALLOCATED 0x60-byte Camera block = the composed output       [C]
+0x70             f32 duration (frames) used by the NEXT dest-setter call                  [C]
+0x74/0x78/0x7C   scalar-C cur/dest/step  <-> Camera+0x1A (s16, as float in handle)       [C]
+0x80/0x84/0x88   EYE current x/y/z                                                       [C]
+0x8C/0x90/0x94   LOOK-AT current x/y/z                                                   [C]
+0x98/0x9C        eye  dest/step X;  +0xA0/0xA4 dest/step Z;  +0xA8/0xAC dest/step Y      [C]
+0xB0/0xB4        at   dest/step X;  +0xB8/0xBC dest/step Z;  +0xC0/0xC4 dest/step Y      [C]
+0xC8 u16 spline progress; +0xCA s16 (60 here); +0xCC ptr -> heap spline block            [C]
   spline block: +0x0 u8 active, +0x2 u16 mode, +0x4 f32 speed, +0x8 ptr waypoint data    [C]
```

### API roles (all [C], from full disasm)
| func | signature | effect |
|---|---|---|
| func_800124A0_130A0 | (h, f32 d) | `h+0x70 = d` (duration for next setter) — `swc1 $f12,0x70($a0)` @0x800124A8 |
| func_80012500_13100 | (h, u16 v) | scalar-C **current** `h+0x74 = (f32)v`, step +0x7C = 0 |
| func_800124AC_130AC | (h, f32 v) | scalar-C **dest** `h+0x78 = v`, step `h+0x7C = |dest-cur|/h[0x70]` |
| func_800127B8_133B8 | (h, x,y,z) | **EYE teleport**: cur ← xyz (+0x80/84/88), eye steps 9C/A4/AC ← 0 |
| func_80012818_13418 | (h, x,y,z) | **LOOK-AT teleport**: cur ← xyz (+0x8C/90/94), at steps B4/BC/C4 ← 0 |
| func_80012518_13118 | (h, x,y,z) | **EYE glide dest** ← xyz (+0x98/A8/A0), steps = \|Δ\|/duration |
| func_80012628_13228 | (h, x,y,z) | **LOOK-AT glide dest** ← xyz (+0xB0/C0/B8), steps = \|Δ\|/duration |
| func_800125F0_131F0 | (h, mode, ptr, f32 speed, s16) | start spline: fills `*(h+0xCC)` block, h+0xC8=0, h+0xCA=s16 |
| func_80012380_12F80 | (h) → bitmask | **poll**: OR of bits for each step still > ε: 1/2/4 = eye X/Y/Z (9C/AC/A4), 8/0x10/0x20 = at X/Y/Z (B4/C4/BC), 0x40 = scalar-C (7C). 0 = arrived |
| func_80012900_13500 | (h, Camera* dst) | **output copy**: 0x60 bytes `*(h+0x6C)[0..0x5C] → dst[0..0x5C]` |
| func_80012878_13478 | (h, Camera* src) | inverse: adopt Camera into handle (copy into `*(h+0x6C)`, then 127B8(eye), 12818(at), 12500(src+0x1A)) — proves eye/at/scalar-C identities |
| func_80012304_12F04 | (h) | destroy: frees `*(h+0x6C)` and `*(h+0xCC)` via func_80014B74(`*(0x8015C5C8)+0xC7FA4`, ptr), then func_80034EF8(h) |
| func_80012AD8_136D8 | (h, ?) | the per-frame handle updater/integrator (2235 lines; steps cur→dest, spline, plus fade-RGB writes). Not called from the target func — runs as the handle object's own update **[I]** |

**func_80014B74_15774(heapCtl, block) = heap free** — [C] from two independent call sites with pointer args (0x8001232C-0x80012334 and 0x80012344-0x8001235C), heapCtl = `*(u32*)0x8015C5C8 + 0xC7FA4`.

### The dynamic eye.x write site
> **Camera.eye.x is written at 0x80012918 — `sw $at, -0xC($t0)` in func_80012900_13500** (first iteration of the 8×0xC copy loop; t0 starts at a1+0xC, so −0xC = Camera+0x0). Value loaded at 0x8001290C from `*(handle+0x6C)+0x0`. Called from the target's shared tail at **0x08000B2C** with a0=`*(s0+0x10)`, a1=`*(s0+0x14)` — every frame the handle is non-NULL. **[C]**

---

## 3. Complete per-case dataflow map

### 3.1 Case 0 (L_080003BC) — init
- call func_8021927C(s1) @0x080003C4 → `s1+0x84 = *(u32*)0x8015CD60` (g84) **[C]**
- `0x080003E0 swc1 $f4,0x8($t7)` → **g84+0x8 = overlay float [seg61+0x15FC]** (X)
- `0x080003F0 swc1 $f6,0xC($t8)` → **g84+0xC = 434.0f** (0x43D90000) (Y)
- `0x08000400 swc1 $f8,0x10($t1)` → **g84+0x10 = 5.0f** (Z)
- `0x08000408 sb $v1,0x64($t2)` → `*( *(0x801FC628) + 0x64 ) = 1` (engine flag; 0x801FC628 = lui 0x8020, lw −0x39D8) **[C]**
- counter=0 (0x0800040C), state=1 (0x08000414).

**g84 = *(0x8015CD60) field map** (this function's view):
| offset | type | written | read |
|---|---|---|---|
| +0x8 | f32 X | 0x080003E0 | 0x080004AC, 0x08000548, 0x08000620, 0x080006A4, 0x080007E4 |
| +0xC | f32 Y | 0x080003F0 | 0x080004D0, 0x0800056C, 0x08000638, 0x080006A8, 0x080007E8 |
| +0x10 | f32 Z | 0x08000400 | 0x080004EC, 0x08000588, 0x08000650, 0x080006B0*, 0x080007F0* (*delay slots) |
| +0x14 | u16 | 0x080005F4 ← 0 | — |
| +0x16 | u16 | 0x080005E4 ← 0xC0 | 0x08000668 (copied to mirrors) |
| +0x18 | u16 | 0x08000604 ← 0 | — |

The trio +0x8/+0xC/+0x10 is used strictly as a **world-space point** (fed as x,y,z args to the interpolator and to debris placement): it is the quake epicenter / camera attention point. The halfwords at +0x14/16/18 mirror the rotation-slot positions of the engine transform block (§6) — rotation/param halfwords **[I]**.

### 3.2 Case 1 (L_08000418) — earthquake: rumble, debris, wait for event
- counter==1: call func_80038BC8(0x24D) @0x0800042C (small-ID trigger; SFX **[I]**); `sb 0 → s0+0x8` (0x08000434).
- while counter < 0x51 (81): every 8 frames two debris spawns (§3.2a).
- Every frame, trigger check: `0x080005A0 lw $t7,0x0($a3)` (a3=0x801FC604 via lui 0x8020/addiu −0x39FC), `0x080005AC lhu $t8,0x98($t7)` — **gate: `*(u16*)( *(0x801FC604) + 0x98 ) != 0`**. Until then, fall through to §3.2b.
- On trigger (once):
  - func_80038BC8(0x17) @0x080005B8 (SFX **[I]**)
  - `0x080005D4 sb $t1,-0x51E0($at)` → `*(u8*)( *(0x8015C5C8) + 0x3AE20 ) = 2` (at = 0x40000 + heap base; game-state flag) **[C]**
  - `0x080005E4 sh $t0,0x16($t3)` → g84+0x16 = 0xC0; `0x080005F4` g84+0x14 = 0; `0x08000604` g84+0x18 = 0
  - `0x08000614 sh $t0,0x94($t6)` → `*( *(0x801FC604) + 0x94 ) = 0xC0`
  - **Mirror copy** of g84 pose into TWO engine camera-state blocks (`*(0x801FC614)` "mirrorA", `*(0x801FC61C)` "mirrorB", loaded via lui 0x8020 / lw −0x39EC and −0x39E4):
    - +0x8: 0x08000624 (A), 0x0800062C (B) ← g84+0x8 (loaded 0x08000620)
    - +0xC: 0x0800063C (A), 0x08000644 (B) ← g84+0xC
    - +0x10: 0x08000654 (A), 0x0800065C (B) ← g84+0x10
    - +0x16: 0x0800066C (A), 0x08000674 (B) ← g84+0x16
  - func_801DACDC(`*(0x801FC604)`, 0x55) @0x08000680 → func_801DAD68(obj, 0x55, 0) — set actor animation 0x55 **[I]**
  - counter=0 (0x08000688), state=2 (0x08000690).
- §3.2b every frame of case 1: **func_80012818(handle, g84.X, g84.Y, g84.Z)** @0x080006AC (args loaded 0x080006A0-0x080006B0) = pin the LOOK-AT at the epicenter; then shared tail (§3.7).

#### 3.2a Debris spawn placement (the "RNG" writes — NOT camera shake)
At counter%8==0 (and a +rand variant at counter%8==4):
- `v0 = func_8021804C(s1, 0)` @0x0800046C → func_802171A8(s1, spec 0x80211EF8, 8) → **spawns a child object** positioned from the parent's transform `*(s1+0x18)`; `s0+0x4 = v0` (0x08000478); `*(v0+0x64) |= 2` (0x0800048C).
- rand = func_802192B4(10) (bounded RNG), then writes go to the **child's transform block** `t7 = *( *(s0+0x4) + 0x18 )`:
  - `0x080004C0 swc1 $f4,0x8($t7)` → child pos.x = g84.X − rand
  - `0x080004D8 swc1 $f6,0xC($t2)` → child pos.y = g84.Y
  - `0x080004FC swc1 $f18,0x10($t4)` → child pos.z = g84.Z − rand
  - (+rand variant: 0x0800055C / 0x08000574 / 0x08000594)

**[C] These stores never touch any camera struct** — they scatter debris/effect actors around the epicenter. The prior note "0x802192B4 = camera shake" is wrong for this function.

### 3.3 Case 2 (L_080006BC) — camera flight
- counter==1 only:
  - func_800124A0(h, 50.0f) @0x080006D8 (duration=50; fn ptr saved to sp+0x28 in the delay slot 0x080006DC)
  - func_80012628(h, g84.X, g84.Y, g84.Z) @0x080006F8 — glide LOOK-AT to epicenter over 50 frames
  - func_800124A0(h, 5.0f) @0x0800070C (duration=5 for the next setter)
  - func_800125F0(h, 2, overlay[seg61+0x1360], 1.0f, 0x3C) @0x08000734 — start **waypoint spline** (mode 2, speed 1.0, aux 60) over overlay data at +0x1360; drives the EYE **[I]**
- Every frame: func_80012380(h) @0x08000744; when it returns 0 → counter=0 (0x08000754), state=3 (0x0800075C).

### 3.4 Case 3 (L_08000760) → 10-frame delay; Case 5 (L_080007A0) → external wait
- Case 3: when counter==0xA: func_8003D388(0x1D4, 1) @0x08000778 and func_80024038(0xC3) @0x08000788 (event/jingle triggers **[I]**); counter=0, **state=5** (0x0800079C).
- Case 5: func_8003F1D8() @0x080007A8; when returns 0 → counter=0, **state=4** (0x080007C0).

### 3.5 Case 4 (L_080007C4) — final glide to a fixed pose
- counter==1 only:
  - func_80012818(h, g84 X/Y/Z) @0x080007EC — LOOK-AT teleport to epicenter
  - func_800124A0(h, 30.0f) @0x08000800 — duration=30
  - func_800124AC(h, 5120.0f) @0x08000814 — scalar-C dest (the Camera+0x1A channel) → 5120
  - func_80012518(h, 1823.0f, 184.0f, 24.0f) @0x08000834 — **EYE glide dest** (0x44E3E000/0x43380000/0x41C00000)
  - func_80012628(h, 1725.0f, 166.0f, 3.0f) @0x08000854 — **LOOK-AT glide dest** (0x44D7A000/0x43260000/0x40400000)
- Every frame: func_80012380(h) @0x08000864; when 0:
  - func_80012304(h) @0x08000878 — destroy handle (frees its heap blocks)
  - `0x08000880 sw $zero,0x10($s0)` — handle = NULL (tail stops calling 12900)
  - `0x08000894 sb $zero,0x64($t1)` → `*( *(0x801FC628)+0x64 ) = 0`
  - **Literal Camera fill** (cam = `*(s0+0x14)`, loaded fresh before each store):
    | vram | store | field | value |
    |---|---|---|---|
    | 0x080008A4 | `sh $t2,0x1A($t3)` | cam+0x1A | 0x1400 (=5120 — matches the 124AC dest!) |
    | **0x080008B4** | `swc1 $f8,0x0($t4)` | **cam eye.x** | **1824.0f** (0x44E40000) |
    | 0x080008C4 | `swc1 $f10,0x4($t5)` | eye.y | 167.0f (0x43270000) |
    | 0x080008D0 | `swc1 $f18,0x8($t6)` | eye.z | 27.0f (0x41D80000) |
    | 0x080008E4 | `swc1 $f4,0xC($t7)` | at.x | overlay float [seg61+0x1600] |
    | 0x080008F4 | `swc1 $f6,0x10($t8)` | at.y | 168.0f (0x43280000) |
    | 0x080008FC | `swc1 $f16,0x14($t9)` | at.z | 8.0f (0x41000000) |
  - counter=0 (0x08000900), **state=6** (0x08000908).
  - Note eye literal (1824,167,27) ≈ 12518 dest (1823,184,24) and at literal (≈?,168,8) vs 12628 dest (1725,166,3): the fill parks the camera at (nearly) the glide destination.

### 3.6 Case 6 (L_0800090C) — teardown & self-destruct (runs once, at counter==1)
- `0x08000934 sb $zero,-0x51E0($at)` → clear `*( *(0x8015C5C8)+0x3AE20 )`
- func_801DACDC(`*(0x801FC604)`, 0) @0x08000940 — reset actor animation **[I]**
- `0x08000958 sb $zero,0x64($t4)` → `*( *(0x801FC628)+0x64 ) = 0`
- `0x08000968 sb $t5,0xD0($t6)` → `*( *(0x801FC624) + 0xD0 ) = 0x1E` (30) — other scene object's field **[C]** (meaning **[I]**: hand-off timer/state)
- Camera fill #1 (identical to §3.5 but at.x = overlay[seg61+**0x1604**]): sh@0x08000978 (+0x1A=0x1400), **eye.x@0x08000988**, eye.y@0x08000998, eye.z@0x080009A0, at.x@0x080009B4, at.y@0x080009C4, at.z@0x080009D8 (jalr delay slot)
- func_80221FB0() @0x080009D4 — clears u8 0x800C7AE2, u8 0x8015C562, and `*(u16*)( *( *(0x801FC604)+0x5C ) ) = 0` **[C]** (engine cutscene/camera-lock release **[I]**)
- re-clear `*( *(0x801FC628)+0x64 )` @0x080009EC; Camera fill #2 (at.x = overlay[+**0x1608**]): eye.x@0x08000A0C, at.x@0x08000A38, at.z@0x08000A50
- func_80221ED4() @0x08000A5C — `*(u32*)0x8015CD04 &= ~1` (flag clear at 0x8015CC30+0xD4) **[C]**
- re-clear +0x64 @0x08000A74; Camera fill #3 (at.x = overlay[+**0x160C**]): eye.x@0x08000A94, at.x@0x08000AC8, at.z@0x08000AE8
- **func_80014B74(`*(0x8015C5C8)`+0xC7FA4, s0)** @0x08000AF8 (a1=s0 set at 0x08000AB0 `or $a1,$s0,$zero`) — **frees the state block s0 itself** **[C]**
- func_80034ED4() @0x08000B08 — deletes the current object **[I]** (must, or next frame would re-run and double-free — the exit path 0x08000B10 skips the counter increment, leaving counter==1)
- exits via L_08000B44 (no tail, no counter++).

The triple fill with three different overlay floats (+0x1604/8/C) interleaved with the release calls is compiler-faithful; the data words are probably near-identical at-x values (not dumpable statically here — open question).

### 3.7 Shared tail (L_08000B18/L_08000B1C) — the per-frame camera commit
```
0x08000B18  lw   $a0, 0x10($s0)      ; handle
0x08000B24  beql $a0, $zero, skip    ; only if handle != NULL:
0x08000B2C  jalr (0x80012900)        ;   func_80012900(handle, *(s0+0x14))
0x08000B30  lw   $a1, 0x14($s0)      ;   -> copies *(h+0x6C)[0..0x5C] into the Camera
0x08000B38  addiu/sh                 ; *(s1+0x8A) += 1
```
**[C] Every store into the submitted Camera during cases 0–5 goes through this one call** (write site of eye.x = 0x80012918, see §2). Cases 4/6 additionally write the Camera directly (literal fills above).

---

## 4. Submitted Camera vs persistent camera-state — store classification

| target | stores | class |
|---|---|---|
| `*(s0+0x14)` Camera (eye+0/at+0xC/s16+0x1A) | literal fills §3.5/§3.6 + per-frame copy via func_80012900 (@0x08000B2C) | **SUBMITTED render Camera** (layout matches validated struct exactly; submitted-pointer identity [I]) |
| g84 = `*(0x8015CD60)` | +0x8/+0xC/+0x10 defaults (case 0), +0x14/16/18 halfwords (case 1) | **persistent camera-state / transform** (survives across handles) |
| `*(0x801FC614)`, `*(0x801FC61C)` | +0x8/+0xC/+0x10/+0x16 mirror copies (case 1) | engine camera-system state blocks, allocated by func_801CB5D0_5874E0 (`sw $v0,-0x39EC($at)` @0x801CB6C0, `sw $v0,-0x39E4($at)` @0x801CB6E0) **[C alloc site]**; role = default-camera current/target pose pair **[I]** |
| `*( *(s0+0x4)+0x18 )` child transform | debris pos writes §3.2a | world-object transforms, NOT camera |
| misc engine flags | `*(0x801FC628)+0x64`, `*(0x801FC604)+0x94/+0x98(read)`, heap+0x3AE20, 0x8015CD04 bit0, 0x800C7AE2, 0x8015C562 | cutscene bookkeeping |

---

## 5. "Internal camera state" answer (task Q3)

- There is **no yaw/distance/height decomposition in this writer** — it operates on absolute world points end-to-end. No s16 binang is ever fed to sin/cos (no call to 0x80003E10 or any trig). The 0x80012xxx family is a linear/spline interpolator (§2), not vec math.
- The only angle-like fields: g84+0x14/16/18 (u16 trio written 0/0xC0/0) — positionally the rotation slots of the engine transform layout **[I]**; and Camera+0x1A driven to 0x1400 (5120) through the handle's scalar-C channel (+0x74/78/7C) — a zoom/perspective parameter, not an orbit angle **[I]**.
- Consequence for the analog-cam hunt: the orbit/follow math (binang yaw + sin/cos + distance/height) is **not** in this overlay state machine; it lives in the default camera system (see §6 next targets).

## 6. PLAYER POSITION (task Q4 — primary goal)

**Negative result, exhaustively verified [C]:** every value stored to Camera eye/at (directly or via the handle) traces back to:
1. immediate float constants (1824/167/27/168/8/1823/184/24/1725/166/3/5120/50/30/5/1.0),
2. overlay-61 read-only floats (+0x15FC, +0x1600, +0x1604, +0x1608, +0x160C) and waypoint data (+0x1360),
3. g84 fields that case 0 itself set from (1)+(2),
4. bounded RNG (debris only, never camera).

No load of any actor's live position feeds the camera in this function. **This is a fixed scripted camera.**

**The structural payoff — where object positions actually live [C]:**
`func_802171A8_5D2678` (the object spawner called via func_8021804C) reads the parent's spawn transform like this:
```
0x802171BC  lw   $v1, 0x18($a0)     ; v1 = *(obj+0x18)  — transform block
0x802171E8  lwc1 $f4, 0x8($v1)      ; pos.x
0x802171F8  lwc1 $f6, 0xC($v1)      ; pos.y
0x80217208  lwc1 $f8, 0x10($v1)     ; pos.z
0x80217218  lhu  $t8, 0x14($v1)     ; rot[0]
0x80217228  lhu  $t9, 0x16($v1)     ; rot[1]
0x80217234  lhu  $t0, 0x18($v1)     ; rot[2]
```
and the debris placement writes through the same chain on the spawned child (§3.2a). So the engine convention is:

> **object world position = `*( *(obj + 0x18) ) + 0x8/0xC/0x10` (f32 x,y,z), rotation = u16 trio at +0x14/0x16/0x18.**

This directly explains the earlier live-dump miss: the movement-state object `*(0x8020CA2C)` keeps its position **behind the pointer at +0x18**, not inline in its first 0xC0 bytes. **[I → high-priority live check: dump `*( *(0x8020CA2C)+0x18 )+0x8..0x10`.]**

**Candidate holder of the followed/player actor:** `*(u32*)0x801FC604` — the scene's principal-actor slot: this cutscene gates on its +0x98 event halfword, writes its +0x94, sets its animation via func_801DACDC, and 150+ overlay functions across all areas reference it (scan §7). Its position would be `*( *(0x801FC604)+0x18 )+0x8/0xC/0x10` **[I]**. (No `sw` to −0x39FC(0x8020-lui) exists anywhere in RecompiledFuncs — the global is assigned through computed addressing, so its writer wasn't pinned statically.)

**Recommended next static targets for the real follow-camera eye-writer:** the 0x801CBxxx–0x801CCxxx cluster: func_801CB5D0_5874E0 (allocates the 0x801FC614/61C camera blocks), func_801CB824_587734, func_801CC000_587F10 and func_801CC4C0_5883D0 (both flagged for handle-like cur/dest/step field usage), func_801D8290_5941A0 (4 refs to mirrorA) — and note the validated movement resolver funcs 801CE3F0/801CE4D0 sit in this same cluster and read `*(0x801FC624)`.

## 7. Corrections / additions to the KNOWN-FACTS list

1. **"124 → 0x80031790" is wrong.** No func_80031790 exists in funcs.h. Line 124's target builds `lui 0x8004; addiu −0x7438` = **0x80038BC8** — identical constants to line 408 (which the list itself resolves to 0x80038BC8). Called as (0x24D) and (0x17): SFX/event trigger **[I]**.
2. **"0x802192B4 bounded RNG = camera shake" is misleading here:** the RNG offsets position spawned debris actors (§3.2a); no camera field is randomized in this function.
3. func_8021804C is an object **spawner** (via func_802171A8 + func_800358E8), not a finder: a1=0 → 8-entry spec table 0x80211EF8; a1≠0 → 13-entry table 0x802139E0 **[C]**.
4. func_8021928C_5D475C is a duplicate of func_8021927C (same `*(0x8015CD60) → *(a0+0x84)`) — only 3 functions in the whole binary reference 0x8015CD60: these two + overlay func_0800221C_6C196C **[C]**.
5. func_80014B74 = heap **free**(heapCtl, block), heapCtl = `*(0x8015C5C8)+0xC7FA4`; `*(0x8015C5C8)` is the master arena (also hosts the game-state byte at +0x3AE20) **[C]**.
6. The 0x80012304–0x80012940 family = camera-path interpolator module; full layout in §2 **[C]**.
7. Handle scalar-C channel maps to **Camera+0x1A** (via func_80012878's adoption: `lhu a1,0x1A(src)` → func_80012500) — and both case-4's 124AC(5120.0) and the literal `sh 0x1400` agree on 5120 **[C]**.

## 8. Open questions

- Identity of `*(0x801FC604)` (player vs. cutscene NPC slot) — needs a live dump or writer hunt (assigned via computed addressing, not a direct lui/sw pair).
- Where the handle `*(s0+0x10)` and Camera `*(s0+0x14)` pointers are created (a sibling overlay function / area-init runs before state 0; not in this function).
- Values of overlay floats seg61+0x15FC/0x1600–0x160C and the waypoint array at +0x1360 (static data not extracted).
- Exact consumer semantics of Camera+0x1A = 5120 (zoom/perspective parameter?) and of g84+0x16 = 0xC0.
- func_80034ED4 assumed "delete current object" (arg-less) — inferred from the double-free-avoidance argument (§3.6), not from its body.
- Which engine routine calls func_80012AD8 for this handle (integration timing).

## 9. File inventory (all in scratchpad)

- target_func_0800037C.c (input), helper_*.c (input)
- api_func_80012900_13500.c, api_func_80012818_13418.c, api_func_80012628_13228.c, api_func_80012518_13118.c, api_func_800124A0_130A0.c, api_func_800124AC_130AC.c, api_func_800125F0_131F0.c, api_func_80012380_12F80.c, api_func_80012304_12F04.c, api_func_80012878_13478.c, api_func_800127B8_133B8.c, api_func_80012500_13100.c, api_func_80012AD8_136D8.c, api_func_80012940_13540.c (+ smaller neighbors), api_func_802171A8_5D2678.c, api_func_8021928C_5D475C.c
- hunt.py / attrib.py — cross-reference scans (global refs by function; integrator candidates)
