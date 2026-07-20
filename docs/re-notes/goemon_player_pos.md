# Goemon64 — Player World Position (writer-side trace)

Static RE over `RecompiledFuncs/funcs_*.c` (WSL: `~/projects/Goemon64Recomp`). All addresses are VRAM; disasm quoted from the recompiled C comments.

## TL;DR — the answer

**CONFIRMED: player world position = `*(u32*)0x801FC60C` → `+0x8` (x), `+0xC` (y), `+0x10` (z), three f32.**

Equivalently: `pos_node = *( *(u32*)0x801FC604 + 0x18 )`; the global `0x801FC60C` is just a cached copy of `player_ctrl->0x18`, set once at player spawn.

- `*(u32*)0x801FC604` = player **control/physics object** ("player_ctrl"): velocity vec3f at `+0xC0/+0xC4/+0xC8`, movement-mode byte at `+0x90`, ptr to control block `0x8020C860` at `+0x5C`, position-node ptr at `+0x18`.
- `*(u32*)0x801FC60C` = player **position node** (head of a linked chain `node+0x0 → next`, every node carrying a vec3f position at `+0x8/+0xC/+0x10`).

## 1. Who writes the globals (spawn site)

`func_801CB5D0_5874E0` (funcs_26.c, ~line 153) — player spawn/init:

```
0x801CB668: jal  0x800358E8            ; allocate/spawn object -> v0
0x801CB670: lui  $s0, 0x8020
0x801CB674: addiu $s0, $s0, -0x39FC    ; s0 = 0x801FC604
0x801CB680: sw   $v0, 0x0($s0)         ; *(0x801FC604) = player_ctrl   [WRITER]
0x801CB678/7C: v1 = 0x8021<<16 - 0x37A0 = 0x8020C860
0x801CB684: sw   $v1, 0x5C($v0)        ; player_ctrl->0x5C = 0x8020C860 (control block)
0x801CB6AC: lw   $t1, 0x18($a0)        ; t1 = player_ctrl->0x18
0x801CB6B8: sw   $t1, -0x39F4($at=0x8020<<16)  ; *(0x801FC60C) = pos node  [WRITER]
0x801CB6C0: sw   $v0, -0x39EC($at)     ; *(0x801FC614) = result of func_80035EEC (companion obj #1)
```

Only stores to `0x801FC60C` in the whole codebase are funcs_26.c:290/294 (this site). `0x801FC604` (`-0X39FC` under `lui 0x8020`) is read ~400 times across base exe and per-area overlays — it is THE player-object global.

### About `0x8020CA2C` (task item 1)

The known "movement-state object ptr" is **not** a heap player object. Its writer is `func_801CC4C0_5883D0` (funcs_23.c):

```
0x801CC690: lui  $t9, 0x8009
0x801CC6AC: addiu $t9, $t9, -0x3340   ; t9 = 0x8008CCC0
0x801CC780: lui  $at, 0x3 ; ori $at, 0xB0F0   ; 0x3B0F0
0x801CC788-98: t1 = (mode*4 - mode)<<3 + 0x3B0F0 + t9   ; mode = *(u8*)(player_ctrl+0x90)
0x801CC69C/6A0: v0 = 0x8021<<16 - 0x35D4 = 0x8020CA2C
0x801CC79C: sw   $t1, 0x0($v0)        ; *(0x8020CA2C) = 0x800C7DB0 + 24*mode   [WRITER]
0x801CC7A4..: copy record words +0x0..+0x10 to 0x8020CA30..0x8020CA40
```

So **CONFIRMED**: `*(0x8020CA2C) = 0x800C7DB0 + 24 * movement_mode` — a pointer into a static **movement-parameter table** (24-byte records: speeds/turn rates), re-written every physics tick. That is why the live dump of its first 0xC0 bytes showed dir/speed-ish data but no world position — it is parameter data, not the player. (Refines the earlier "heap ptr" note: the value is a static-data address.)

Readers of `0x8020CA2C`: `func_801CE4D0_58A3E0` (0x801CE508), `func_801CE3F0_58A300` (0x801CE42C, reads `+0xC` float), `func_801CEAD4_58A9E4` (0x801CEAD8, reads `+0x10`, `+0x14` floats), `func_801CD310_589220` (0x801CD37C, reads `+0xC`) — all movement-resolver consumers of the parameters.

## 2. Sibling globals map (0x8020CAxx and 0x801FC6xx)

| Global | Semantics | Evidence |
|---|---|---|
| `0x801FC604` | player_ctrl object ptr | sw @0x801CB680 |
| `0x801FC60C` | player position node ptr (`= player_ctrl->0x18`) | sw @0x801CB6B8 |
| `0x801FC614` / `0x801FC61C` | companion objects (spawned right after player; camera copies its eye pos into their `+0x8/+0xC/...`) | sw @0x801CB6C0; overlay 0x0800061C-0x0800063x |
| `0x801FC624` | default value loaded into `player_ctrl+0x64` when `*(u16*)(obj64+0x5C)==0` | lw @0x801CC528, sw @0x801CC52C |
| `0x801FC628` | object ptr; camera clears its `+0x64` byte | 0x0800094C/0x08000958 in overlay |
| `0x8020CA20/24/28` | scratch: written with **old** player pos (from `*(0x801FC60C)+0x8/C/10`) at physics-tick start, then overwritten with **per-frame delta** after collision | swc1 @0x801CC4F4/504/514; sub.s+swc1 @0x801CD14C-15C, 0x801CD198 |
| `0x8020CA2C` | movement-parameter record ptr (`0x800C7DB0 + 24*mode`) | sw @0x801CC79C |
| `0x8020CA30..40` | copy of current parameter record words 0..4 | 0x801CC7AC-0x801CC7D0 |
| `0x8020CAC0` / `0x8020CAD0` | extra displacement vec3fs added to secondary objects (platform carry?) | lwc1 @0x801CD0BC (`-0x3540`), 0x801CD110 (`-0x3530`) |

Also: heading binang = `*(u16*)(0x8020C860 + 0xA4)` (see §3); `0x8020C860` is reachable both directly and as `player_ctrl->0x5C`.

## 3. Direction → velocity → position (task item 3)

Pipeline (all base-exe, physics side):

1. **Heading integration** — `func_801CEAD4_58A9E4` (funcs_32.c, arg a0=player_ctrl, a1=out vec3f):
   - `0x801CEAF0: lwc1 $f4, 0x10($t6=*(0x8020CA2C))` — turn-rate param × 96.0 → binang step.
   - v0 = `*(s0+0x5C)` = `0x8020C860`; steering delta halfword at `+0xA8` (clamped to ±0x2D0 @0x801CEB5C-0x801CEB90), accumulated into **heading** at `+0xA4` (`0x801CEBA0-A4: addu/sh 0xA4($v0)`).
   - `0x801CEBB0: jal 0x80003CC8` (sin, INFERRED — sibling of known math_sin 0x80003E10) → `swc1 $f0, 0x0($a2)`; `0x801CEBC4: jal 0x80003D28` (cos) → `swc1 $f0, 0x8($a2)`; `0x801CEBDC: swc1 $f16(=0), 0x4($a2)` → **unit direction vector (sin h, 0, cos h)**.
2. **Camera-relative resolution** — known `func_801CE3F0_58A300`/`func_801CE4D0_58A3E0` consume the parameter record and produce velocity into `player_ctrl+0xC0/+0xC4/+0xC8`.
3. **Position integration** — `func_801CC4C0_5883D0` (funcs_23.c), called from `func_801CBF0C_587E1C` (funcs_21.c:8739, `a0=player_ctrl, a1=*(a0+0xE4), a2=*(a0+0xE8)`):
   - Entry caches old pos: `0x801CC4E4: lwc1 $f4, 0x8($v0=*(0x801FC60C))` → `0x801CC4F4: swc1 $f4, -0x35E0($at=0x8021<<16)` (= `0x8020CA20`), same for `+0xC→0x8020CA24`, `+0x10→0x8020CA28`.
   - Integration @0x801CC674-0x801CC6F4: `f = *(node+0x8) + *(a3+0xC0)` → `swc1 0x8($t3)`, likewise `+0xC += +0xC4`, `+0x10 += +0xC8`, applied to the node chain `t6=*(a0+0x18)`, `a2=*(t6+0)`, `v1=*(a2+0)` and to `*(a0+0x64)` — **pos += velocity** on every node.
4. **Delta/speed** — `func_801CD084_588F94` (funcs_22.c:39): `0x801CD148: lwc1 $f4, 0x8($v0=*(0x801FC60C))`; `0x801CD14C: sub.s $f8, $f4, $f6(=old 0x8020CA20)`; stores deltas back into `0x8020CA20/24/28` and computes `dx²+dy²+dz²` (0x801CD1AC-…) — actual per-frame displacement magnitude. This is the function that proves `*(0x801FC60C)+0x8/C/10` is *the* authoritative position (its motion defines "how far the player moved this frame").

## 4. Camera cross-check (task item 4)

In `target_func_0800037C.c` (overlay camera state machine, s1=camera obj, `*(s1+0x84)`=`*(0x8015CD60)` camera block):

- `0x08000598-0x080005A0`: `a3 = 0x801FC604; t7 = *(a3)`; checks `*(u16*)(player_ctrl+0x98)`.
- `0x08000614: sh $t0(=0xC0), 0x94($t6=*(0x801FC604))` — writes player state.
- `0x0800093C-0x08000940`: `a0 = *(0x801FC604); jalr 0x801DACDC` — passes the **player_ctrl object** to the camera-follow helper (the same `func_801DACDC` in the known-callee list). So the live follow logic reaches the player via `player_ctrl` (and INFERRED: its `+0x18` node / `+0x5C` control block), not via a separate camera-side copy.
- `0x0800061C-0x0800063x`: copies `*(*(s1+0x84))+0x8/+0xC/...` (camera block floats) into `*(0x801FC614)+0x8...` and `*(0x801FC61C)+0x8...` — same `+0x8/+0xC/+0x10` vec3f convention on the companion objects.
- `helper_80221FB0.c` (`func_80221FB0_5DD480`): `t6=*(0x801FC604); t7=*(t6+0x5C); sh $zero, 0x0($t7)` — independently confirms `player_ctrl->0x5C == 0x8020C860`.
- The intro-case camera hardcodes eye `(1826, 167, 27)` into `*(s0+0x14)+0x0/4/8` and look_at into `+0xC/...` (0x08000984-0x080009C4) — confirming `*(s0+0x14)` is the render Camera struct (eye `+0x0`, look_at `+0xC`), matching the known Camera layout.

The camera func itself never loads `-0X39F4` (`0x801FC60C`); it goes through `0x801FC604` and the helper 0x801DACDC → 0x801DAD68 chain (which reads `*(s0+0x5C)` and per-player-index tables `0x80203F34[]`, `0x80203B90[]`).

## 5. Practical recipe (for the analog-camera patch)

- Player world position (f32 x/y/z): `p = *(u32*)0x801FC60C; x=*(f32*)(p+0x8); y=*(f32*)(p+0xC); z=*(f32*)(p+0x10)` — valid whenever the player exists (set at spawn, not per-frame).
- Player heading (binang, 0x10000=full turn): `*(u16*)0x8020C904 + …` — precisely `*(u16*)(0x8020C860 + 0xA4)`; steering rate at `+0xA8`.
- Player_ctrl object: `*(u32*)0x801FC604` (velocity `+0xC0..C8`, mode `+0x90`, control block `+0x5C`).
- Per-frame displacement vector: `0x8020CA20/24/28` **after** `func_801CD084` runs (before that, same globals hold last frame's start position).

## Confidence labels

- CONFIRMED: everything in §1, §2 rows 1-2/6-7, §3 steps 3-4, `player_ctrl->0x5C == 0x8020C860`, Camera struct eye/look_at offsets.
- INFERRED: 0x80003CC8/0x80003D28 = sin/cos (from usage pattern + proximity to math_sin); companion objects `0x801FC614/61C` being shadow/marker objects; exact role of `0x8020CAC0/D0` vectors; 0x801DAD68 reading position through the node chain.

## Open questions

- Which function fills velocity `player_ctrl+0xC0..C8` from the resolved direction (between func_801CEAD4's unit vector and func_801CC4C0's integration) — likely in the func_801CE4D0 body past line 186 region; not fully traced.
- Whether the node chain `*(player+0x18) → +0x0 → +0x0` nodes are body-part transforms or shadow/collision proxies (all get the same +velocity).
- y-delta gating at 0x801CD160-0x801CD19C (byte flags `obj5C+0x59`, `s0+0x61`) — looks like ground-clamp logic, untraced.
