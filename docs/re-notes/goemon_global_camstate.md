# Goemon64 RE: global camera-state `*(0x8015CD60)` and the movement resolver's camera basis

Static analysis of `~/projects/Goemon64Recomp/RecompiledFuncs` (all 148 files scanned per query; every claim below carries disasm evidence `address: instruction`). CONFIRMED = direct instruction evidence; INFERRED = marked as such.

---

## 0. Corrections to task assumptions (important)

1. **"0x8020C628" is actually 0x801FC628.** The task's own construction (`lui 0x8020` + lo16 `-0x39D8`) yields `0x80200000 - 0x39D8 = 0x801FC628`. Same for its neighbors: `-0x39FC → 0x801FC604`, `-0x39F4 → 0x801FC60C`, `-0x39DC → 0x801FC624`. All "0x8020C6xx" labels in prior notes are off by +0x10000; the real cluster is **0x801FC6xx**. (The 0x8021-based globals are unaffected: `lui 0x8021` + `-0x35D4` = 0x8020CA2C etc., as known.)
2. **`*(0x8020CA2C)` is not a heap object.** It is a pointer into a **static in-RAM table of 0x18-byte camera-direction records at 0x800C7DB0** (see §5). "Valid ptr only during gameplay" from the live dump is consistent: the store executes only while the gameplay module runs.

---

## 1. All references to 0x8015CD60 (readers / writers / allocation)

Exhaustive scan for lo16 `-0X32A0` (all files, both orders of `MEM_W`; also catches `addiu` address-takes). **Exactly 3 references exist, all LOADS. There is no writer anywhere in the recompiled code.**

| # | Function | File | Evidence | Role |
|---|----------|------|----------|------|
| 1 | `func_8021927C_5D474C` | funcs_41.c:4117 | `0x8021927C: lui $t6,0x8016` / `0x80219280: lw $t6,-0x32A0($t6)` / `0x80219288: sw $t6,0x84($a0)` | getter stub: `a0->0x84 = *(0x8015CD60)` |
| 2 | `func_8021928C_5D475C` | funcs_52.c:95 | identical twin at 0x8021928C..98 | same |
| 3 | `func_0800221C_6C196C` | funcs_108.c (overlay) | `0x08002234: lui $v0,0x8016` / `0x08002240: lw $v0,-0x32A0($v0)` | overlay reader (see §3) |

**Allocation site: none in code.** Checks performed: no `sw` with that lo16; no `addiu`-computed address-of (would still contain `-0X32A0`); no `ori 0xCD60` idiom. Since both getter stubs expect a valid pointer, the slot must be populated by the **initial data image loaded at boot**, not by any C-level store. Section mapping (`Goemon64RecompSyms/mnsg.datasyms.toml`): `D_8015CD60` falls in the catch-all section `.audio_tables` (vram 0x800967C0..0x80556B40, boot delta 0x7FFFF400 → rom 0x15D960); a raw ROM dump at that offset decodes as audio-like bytes, so the reconstructed-ELF linear mapping is unreliable for this region and the true initial value could not be verified statically (open question). VERDICT (INFERRED, high confidence): **0x8015CD60 is a pre-initialized data pointer to a static camera-parameter block; it is never re-pointed at runtime**, so struct size cannot be derived from an allocation call — layout comes from access sites (§3).

Getter callers: base-exe `func_80218A54_5D3F24` (funcs_55.c:3447, a spawn-time object initializer — `0x80218BEC: jal 0x8021928C`, right after writing `s0+0x68/+0x6C` floats) and dozens of overlay call sites via `lui 0x8022 / addiu -0x6D84` (= 0x8021927C) in funcs_105/106/114/121/… — i.e. **every camera-flavored object stores this pointer at obj+0x84 at spawn**, including the overlay camera state machine `func_0800037C_72E5DC` (case 0, `0x080003C4: jalr $t9` → 0x8021927C).

Neighborhood map (scan of `0x8016`/`-0x32xx`, 156 accesses, 27 addresses): 0x8015CDxx is a misc base-exe globals area — e.g. 0x8015CDB8 is a pointer loaded as first arg 64× by menu-ish overlays (funcs_142), 0x8015CDE0 is an asset-lookup pointer variable (`func_80218A54`: `0x80218A88: sw $a0,0x0($a3)` then `0x80218A94: sw $v0,0x0($a3)` after `func_80014840_15440`). Nothing else in the window relates to the camera-parameter block.

---

## 2. THE ANSWER — where the movement resolver gets its camera basis

### 2.1 `func_801CE3F0_58A300` (funcs_32.c:3003) — entry, magnitude gate, record normalization

- Clears out-vector `a0+0x74/78/7C` (`0x801CE404..0C: swc1 $f14`).
- Loads **`v0 = *(0x8020CA2C)`** — `0x801CE414: lui $v0,0x8021` / `0x801CE42C: lw $v0,-0x35D4($v0)` — the **camera-direction record** (see §5), and reads:
  - `0x801CE434: lwc1 $f0,0xC($v0)` — **record+0xC = length** of the direction vector;
  - `0x801CE47C: lwc1 $f2,0x10($v0)`, `0x801CE480: lwc1 $f12,0x14($v0)` — **record+0x10/+0x14 = planar direction components**;
  - normalizes with a sign flip: `0x801CE48C: neg.s $f2` then `div.s f2,f2,f0` / `div.s f12,f12,f0` → passes as `a2 = -d1/len`, `a3 = d2/len` into `func_801CE4D0` (`0x801CE4B0: jal 0x801CE4D0`), stick magnitude via `sp+0x10`.
- Dead-stick path touches `*(0x801FC624)+0xAB` (`0x801CE460: lw $t1,-0x39DC($t1)` / `0x801CE468: sb $zero,0xAB($t1)`) and `*(a0+0x5C)+0x78 = 0`.

### 2.2 `func_801CE4D0_58A3E0` (funcs_29.c:153) — **reads the live render Camera struct directly**

Pointer chain (CONFIRMED):

```
0x801CE4EC: lw   $a2, 0x64($a0)      ; a2 = player->0x64  = camera controller object ("objA")
0x801CE4F8: lw   $a3, 0x18($a2)      ; a3 = objA->0x18    = camera transform/view node (== *(0x801FC628), §4)
0x801CE504: lw   $v0, 0x2C($a3)      ; v0 = node->0x2C    = pointer to the render Camera struct
0x801CE4F0/F4: lui $at,0x8FFF / ori $at,0xFFFE
0x801CE510: and  $t6, $v0, $at       ; mask 0x8FFFFFFE — normalizes a segmented (0x0B/0x08) pointer
```

Then it computes **eye − look_at** per component from the known Camera layout (+0x00 eye Vec3f, +0x0C look_at Vec3f):

```
0x801CE514: lwc1 $f4,0x0($t6) ; 0x801CE518: lwc1 $f6,0xC($t6)  ; 0x801CE520: sub.s → sp+0x4C  (x)
0x801CE52C: lwc1 $f10,0x4($t6); 0x801CE528: lwc1 $f16,0x10($t6); 0x801CE530: sub.s → sp+0x50  (y)
0x801CE53C: lwc1 $f4,0x8($t6) ; 0x801CE538: lwc1 $f6,0x14($t6) ; 0x801CE540: sub.s → sp+0x54  (z)
```

It also re-reads the record for the yaw: `0x801CE548: lwc1 $f14,0x10($v1)` / `0x801CE550: lwc1 $f12,0x14($v1)` (v1 = `*(0x8020CA2C)`, `0x801CE508`), calls `func_800040A8_4CA8` (atan2 → binang, `0x801CE554: sh $v0,0x44($sp)`), normalizes the eye−at vector (`func_8001D394_1DF94(sp+0x4C)`), transforms (`func_8001D460_1E060(a0=char+0x6C, a1=sp+0x5C, a2=sp+0x4C)`).

Camera-cut hysteresis: if `*(0x801FC624)+0xAB` (`0x801CE574: lw $t7,-0x39DC($t7)` / `0x801CE578: lbu $t8,0xAB($t7)`) is set → blend counter `char+0x78 = 0x4F` (`0x801CE58C: sb $t9,0x78($t0)`); if record length < 0.3 (`0x801CE5FC: ldc1 $f10,-0x5690($at)`, double at 0x8020A970 = 0.3 verified in ROM at file_11 rom 0x5C6880) or yaw delta < ~11/1024 turn (`0x801CE630: andi 0x3FF` / `slti 0xB`) → **fast path**: store the normalized eye−at basis into `char+0x6C/+0x70/+0x74` (`0x801CE644..54: swc1 → 0x0/0x4/0x8($s0)`, s0 = `*(a0+0x5C)+0x6C`), new yaw into `char+0x6A` (`0x801CE660: sh $t7,0x6A($t8)`); otherwise angle-blend the old and new basis over the counter (sin/cos via `func_80003CC8_48C8`/`func_80003D28_4928`, blend `func_8001D5B8_1E1B8`).

Since init sets `*(player=*(0x801FC604))+0x5C = 0x8020C860` (§4), the **movement camera basis lives at 0x8020C8CC (=0x8020C860+0x6C), yaw at 0x8020C8CA, blend counter 0x8020C8D8** — inside the block previously labeled "camera params 0x8020C860".

### 2.3 `func_801CE870_58A780` (funcs_24.c:3963) — final resolve

Called with `a2 = player+0x84` (`0x801CE840: addiu $a2,$s1,0x84`) and `a3 = char+0x6C` basis. It **normalizes the Vec3f at player+0x84 in place** (`0x801CE888: jal 0x8001D394` with `a0=a2`) — so in the *player* object +0x84 is an inline Vec3f (prev/facing direction), NOT the camstate pointer that +0x84 holds in *camera-type* objects. It computes the record-direction yaw (`0x801CE898: jal 0x800040A8` on the normalized record dir passed on the stack), transforms through the live basis (`func_8001D898_1E498(&basiscopy, out, player+0x84)`, `func_8001D460_1E060`), quantizes yaw to 10 bits (`0x801CE9C8: andi $t0,$v1,0x3FF`), converts angle+magnitude to a world vector (`func_80003F30_4B30` sin/cos pair; `0x801CE9E8/9F4: mul.s`), writes final yaw to `char+0x8` (`0x801CEA44: sh $v1,0x8($t1)`), honors an override flag `char+0x0 & 1` with forced angle `char+0x2` and multiplier `char+0x4` (`0x801CEA50: andi $t3,$t2,0x1`, `0x801CEA60: lhu $a0,0x2($v0)`, `0x801CEA70: lwc1 $f10,0x4($t4)`), and emits the movement vector via `func_8001D718_1E318` into `player+0x74`.

### 2.4 Verdict on the win condition

**The movement resolver NEVER reads `*(0x8015CD60)` or any field of its struct.** Its camera basis comes from exactly two places, both read fresh every frame:

1. **The live render Camera struct** — reached via `*(*(*(player+0x64)+0x18)+0x2C) & 0x8FFFFFFE`, i.e. `*(*(0x801FC628)+0x2C)`; fields +0x00..0x08 (eye) minus +0x0C..0x14 (look_at).
2. **The current camera-direction record** `*(0x8020CA2C)` → fields +0xC (len), +0x10/+0x14 (planar direction) — sourced from the common table 0x800C7DB0 (§5).

Rotating the yaw of the `*(0x8015CD60)` struct cannot rotate the movement basis (it has no yaw; see §3). The complete win requires rotating **both** the Camera eye-about-look_at **and** the record direction (+0x10/+0x14, keeping +0xC consistent) by the same angle — or patching one common choke point: `func_801CE4D0` consumes both, so a recomp patch at its entry (rotate the eye−at delta AND the record dir before use, or equivalently hook `func_801CC4C0` after `0x801CC79C` where `*(0x8020CA2C)`/copy are refreshed, plus the view).

---

## 3. Layout of the struct at `*(0x8015CD60)` (camera-parameter block)

From all access sites found (getter-mediated via obj+0x84 in camera-type objects, plus the overlay reader):

| Offset | Type | Written | Read | Meaning (evidence) |
|--------|------|---------|------|--------------------|
| +0x00 | u32/ptr | — | overlay camera SM (`0x0800061C/34/4C/64: lw ...,0x0($tN)` after `lw $tN,0x84($s1)`) | link/first field, word |
| +0x04 | u32/ptr | — | same pattern (`0x080004A4/E4, 0x08000540/80: lw 0x4`) | second word |
| +0x08 | f32 | case 0: overlay float (`0x080003D0: lwc1 $f4,0x15FC($at)` RELOC → `0x080003E0: swc1 $f4,0x8($t7)`) | overlay SM (`0x080004AC: lwc1 $f10,0x8($t5)`); overlay trigger `func_0800221C_6C196C` range-checks 60.0 < v < 90.0 (`0x08002244: lui $at,0x4270`, `0x08002250: lui $at,0x42B4`) | camera angle parameter in degrees (pitch/elevation), INFERRED |
| +0x0C | f32 | case 0: 434.0f (`0x080003D8: lui $at,0x43D9` → `0x080003F0: swc1 $f6,0xC($t8)`) | (reads inside camera SM cases, via same base) | camera distance, INFERRED |
| +0x10 | f32 | case 0: 5.0f (`0x080003E8: lui $at,0x40A0` → `0x08000400: swc1 $f8,0x10($t1)`) | overlay trigger checks against -90/-65 (`0x08002280: mtc1 0xC2B4`, `0x08002284: lui $at,0xC282`) | speed/height parameter, INFERRED |
| +0x18 | u32 | — | overlay SM (`0x080004CC/0x08000568: lw ...,0x18($tN)`) | word field |

These are scalar camera parameters consumed by the **overlay camera state machine** (which computes the submitted Camera from them) — a tuning block, not a pose. Consumers found only in overlays (per-area camera code) that obtained the pointer through the getters into `obj+0x84`.

---

## 4. `0x801FC628` and the 0x801FC6xx pointer table (task's "0x8020C628")

Cluster map (all built as `lui 0x8020` + negative lo16; classify scan, 876 accesses):

| Global | Refs | Stores (allocation) | Identity |
|--------|------|--------------------|----------|
| 0x801FC604 | 322 | (elsewhere) `func_801CB5D0_5874E0` `0x801CB680: sw $v0,0x0($s0)` | **player/movement object** — spawned by `func_800358E8_364E8` (callback 0x801CB7D4, spawn pos from `*(0x8015C5C8)+0x38000+0x2FE4` halfwords, `0x801CB600: lh $t6,0x2FE4($v0)`); init then sets `player+0x5C = 0x8020C860` (`0x801CB684: sw $v1,0x5C($v0)`, v1 built `0x801CB67C: addiu $v1,$v1,-0x37A0` from lui 0x8021) |
| 0x801FC60C | 257 | same func `0x801CB6B8: sw $t1,-0x39F4($at)` | second spawned object (player-related); `func_801CC4C0` copies its +0x8/+0xC/+0x10 to globals 0x8020CA20/24/28 each frame (`0x801CC4E4: lwc1 $f4,0x8($v0)` → `0x801CC4F4: swc1 $f4,-0x35E0($at)` etc.) |
| 0x801FC614/61C | 27/40 | same func | further player-subsystem objects |
| 0x801FC624 | 44 | `func_801CBD14_587C24` zeroes; **`func_801D23D4_58E2E4` `0x801D24B4: sw $v0,0x0($s0)`** | **camera controller object "objA"** — spawned by `func_80035854_36454` with template 0x8020CBF0 (uncached `0x801D2460: addu $a1,$s0,$at` with `lui 0x2000`), callback 0x801D23C8, params 250.0/15.0 |
| **0x801FC628** | **140** | `func_801CBD14` zeroes; **`func_801D23D4` `0x801D24B8: lw $t0,0x18($v0)` / `0x801D24D0: sw $t0,-0x39D8($at)`** | **= `*(objA+0x18)` — the camera transform/view node.** Its **+0x2C holds the render-Camera pointer** consumed by the movement resolver (§2.2). 130 loads across base modules AND overlays (funcs_92/95/…) — the engine-wide way to reach the active camera node |
| 0x801FC62C/634 | 9/8 | `func_801D23D4` (`0x801D24DC`, `0x801D2558`) | sub-parts of objA obtained via `func_80035EEC_36AEC(objA,2,1)` |

**Byte +0x64 of `*(0x801FC628)`**: overlay camera case 0 sets it to 1 — `0x08000404: lw $t2,-0x39D8($t2)` / `0x08000408: sb $v1,0x64($t2)` (CONFIRMED, and note the lui is 0x8020 → target 0x801FC628). Init performs the same `sb 1,0x64` on the two sibling part-objects (`0x801D2540`, `0x801D25AC`), and the generic object flag word at +0x64 elsewhere is OR'd with 2 before drawing (`func_802127EC_5CDCBC`: `0x80212800: ori $t7,$t6,0x2`). INFERRED: +0x64 is an enable/dirty flag on the node — case 0 "activates" the camera view node.

Init also links **`*(player)+0x64 = objA`** (`0x801D25C0: sw $t4,0x64($t5)`, t5=player from `0x801D25B4: lw $t5,0x0($v1)`, v1=0x801FC604) — closing the resolver's chain `player->0x64->0x18->0x2C` — zeroes the record index `player+0x90` (`0x801D25C8: sb $zero,0x90($t7)`), sets `objA+0x5C = 1` (halfword, `0x801D25D0: sh $a0,0x5C($t8)`) and flag byte `0x800C7AE8 = 1` (`0x801D25E0: sb $a0,0x7AE8($at)`).

---

## 5. The camera-direction record pipeline (what actually feeds movement)

- **Common table at 0x800C7DB0** (low RAM, below the swappable modules — shared across them), 0x18-byte records: bytes 0x0..0xB = halfword fields (angles; `func_8022306C_5DE53C` resets +0x2/+0x4/+0x6/+0x8 as `sh`), bytes +0xC/+0x10/+0x14 = floats {length, dir1, dir2}.
- **Per-frame select** — `func_801CC4C0_5883D0` (movement driver, called by `func_801CBF0C_587E1C`/`func_801CBAF8_587A08`):
  `idx = *(player+0x90)` (`0x801CC758: lbu $t6,0x90($a3)`), `rec = 0x8008CCC0 + 0x3B0F0 + idx*0x18` (`0x801CC690: lui $t9,0x8009`, `0x801CC6AC: addiu $t9,$t9,-0x3340`, `0x801CC788..90: sll/subu/sll` = idx*24, `0x801CC780/84: lui/ori $at,0x3B0F0`), then **`0x801CC79C: sw $t1,0x0($v0)` with v0 = &0x8020CA2C** (`0x801CC680: lui $v0,0x8021` / `0x801CC6A0: addiu $v0,$v0,-0x35D4`) — this is the only writer of 0x8020CA2C — plus a 6-word copy of the record to 0x8020CA30 (`0x801CC7AC..0x801CC7D8`).
  The same function also applies pending world-displacement deltas `player+0xC0/C4/C8` to the transform-node chain positions `+0x8/+0xC/+0x10` (`0x801CC684: swc1 $f4,0x8($t3)` … `0x801CC6F4`), then zeroes the deltas.
- **Table refresh** — camera module `func_80222E4C_5DE31C` copies 4 records from its BSS working set 0x8023B3A0/B3B8/B3D0/B3E8 (`0x80222EA8: addiu $t9,$t9,-0x4C60` from lui 0x8024) into table slots 0..3 (`0x80222EBC: sw $at,0x0($t8)`, t8 = 0x800C7DB0 via `0x80222EB4/B8: lui 0x800C / addiu 0x7DB0`), gated by a byte at `*(0x8015C5C8)+0x3AE23`.
- **Record float writers**: the direction floats are produced inside the camera module (file_12) / per-area overlay camera code through computed record pointers (candidates with the idx*24 pattern: `func_801DECE0_59ABF0` `0x801DED28..38`, `func_801DF3D8_59B2E8` `0x801DF48C..A0`, iterator `func_801CF770_58B680` `0x801CF7D4..E0` with stride 0x18). Exact producer of +0xC/+0x10/+0x14 not pinned down (open question).
- **Index writers** (`player+0x90`): reset by `func_801CC000_587F10` (`0x801CC038: sb $zero,0x90($a0)`), init (`0x801D25C8`), modified by `func_801F09B0_5AC8C0` (`0x801F0A50`, `0x801F0A88`) and `func_801F0DA8_5ACCB8` (`0x801F0E54`) — camera-zone/transition switching.

---

## 6. Item 5 — does `*(0x8015CD60)` feed the submitted render Camera?

- `func_80017D8C_1898C` has exactly two direct callers: `func_80016AA4_176A4` (funcs_2.c:1906) and `func_80016C44_17844` (funcs_19.c:7174). In `func_80016C44` the Camera pointer passed as a0 is the payload of the dispatched display-list command (loop-local `v1`, saved `0x80017218: sw $v1,0x24($sp)`, `0x80017220: lw $a0,0x24($sp)`, `0x80017224: jal 0x80017D8C` with `a1 = 0x801684B0` from `0x8001721C/28: lui 0x8017 / addiu -0x7B50`). **No read of 0x8015CD60 or its struct fields occurs in these callers** — consistent with the known 0x20000000 object-list mechanism.
- The connection is indirect: the overlay camera state machine (func_0800037C etc.) reads the scalar parameters from `*(0x8015CD60)` (angle/distance/speed, §3) and computes the Camera eye/at that it writes into the (overlay-resident) Camera struct it submits. So *(0x8015CD60) parameterizes the camera, but the submitted Camera pose lives in the overlay object, and the movement resolver reads that pose via `*(*(0x801FC628)+0x2C)`.

---

## 7. Practical implications for the analog-camera feature

1. **Single-source rotation is possible without touching per-area overlays**: `func_801CE4D0_58A3E0` is the sole function that converts camera pose → movement basis (its two callers are `func_801CE3F0` ← `func_801CD310_589220` only). A recomp patch there (rotate the eye−at delta *and* the record dir/yaw by the user angle) rotates the movement basis to match a view rotated by the same angle in `func_80017D8C` — replacing v9's render-only rotation with a coherent pair.
2. Alternatively rotate at the data chokepoints each frame after `func_801CC4C0`'s copy (`0x801CC7D8`): the active record `*(0x8020CA2C)` (+0x10/+0x14) — but the live Camera eye−at read in §2.2 must be rotated too, so the function-level hook (option 1) is simpler.
3. `*(0x8015CD60)` is the wrong lever: scalar parameters only, no yaw; and the eye-writer hunt (per-area overlays) remains where the memory note left it — the +0x84-mediated parameter reads found here confirm the overlay camera SM computes the pose from {angle +0x8, distance +0xC, param +0x10}.

## 8. Open questions

- Exact producer of the record floats (+0xC/+0x10/+0x14) in the camera module / overlays (candidates listed in §5); needed only if one wants to rotate at the source instead of at `func_801CE4D0`.
- True initial value of `*(0x8015CD60)` (reconstructed-ELF rom mapping for that region is unreliable); a one-instruction live dump would confirm which static block it points to.
- Semantics of record halfwords +0x0..+0xB (reset by `func_8022306C`; record0+0x4 written by `func_80214F44_5D0414`).
- `func_80016AA4_176A4` (menu-context view caller) not traced in detail.
