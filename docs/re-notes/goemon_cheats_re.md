# Goemon64 — Cheatable Resources (health / money / lives): static RE

Static reverse-engineering sweep over the recompiler output
(`RecompiledFuncs/funcs_*.c`, 510k instructions) plus the section table in
`Goemon64RecompSyms/mnsg.syms.toml`. No emulator, no on-device testing — every
claim below is backed by quoted disassembly. Date: 2026-07-18.

Motivation: the repo **already ships** unimplemented "Infinite Health" and
"Infinite Money" toggles (`assets/config_menu/cheats.rml`,
`include/goemon_config.h`, `src/ui/ui_config.cpp`, `src/game/config.cpp`) with
**no consumer anywhere** — nothing reads those settings to write memory. This
document supplies the addresses needed to wire them up, plus lives.

---

## TL;DR — the answers

All four resources live in one always-resident base-exe block. **No overlay
guard is needed** to write them.

| Resource | Live address | Width | New-game init | Cap / floor |
|---|---|---|---|---|
| **MAX health** | `0x8015C5E0` | `s32` (`lw`/`sw`) | 10 | no writer-enforced cap; HUD clamps display at 40 |
| **CURRENT health** | `0x8015C5E4` | `s32` (`lw`/`sw`) | 10 | clamped to MAX on heal, to 0 on death |
| **MONEY (ryo)** | `0x8015C5E8` | `s32` (`lw`/`sw`) | 100 | **hard cap 9999 (`0x270F`), floor 0** |
| **LIVES** | `0x8015C5EC` | `s32` (`lw`/`sw`) | 3 | decrement-to-0 = game over; restore path floors at 3 |
| revive item | `0x8015C5F0` | `s32` | 0 | (secondary, see §6) |

Two mirrored blocks, both in **base-exe BSS below `0x801CB460`** (always
resident — see §1 for why that matters):

- **LIVE stat block: `0x8015C5D8`, 0x30 bytes** — what gameplay code reads and writes.
- **SAVE buffer ("gamedata"): `0x8015C608`, 0x304 bytes** — Controller Pak backing store, with a shadow copy at `0x8015C910`.
- Mirror relation: **`save_addr = live_addr + 0x94`** (live block sits at save `+0x64`).

| Resource | Live | Save mirror |
|---|---|---|
| MAX health | `0x8015C5E0` | `0x8015C674` (save `+0x6C`) |
| CURRENT health | `0x8015C5E4` | `0x8015C678` (save `+0x70`) |
| MONEY | `0x8015C5E8` | `0x8015C67C` (save `+0x74`) |
| LIVES | `0x8015C5EC` | `0x8015C680` (save `+0x78`) |

---

## 1. Why the address space forced this conclusion (structural finding)

This game uses **three overlay slots**, each hosting many mutually-exclusive
files. Parsed from the `[[section]]` table (one entry per file, `size` = whole
file):

| Slot | VRAM | Files |
|---|---|---|
| A | `0x801CB460` | file_11 (rom 0x587370), file_13 (rom 0x5F6840), file_14, file_15, file_16, file_20, file_22, file_23 |
| B | `0x8020D2A0` | file_12 (rom 0x5C8770), file_17, file_18, file_19, file_21 |
| C | `0x08000000` | file_24 .. file_80 (rom 0x6AC550+) — the `func_08xxxxxx_7xxxxx` actor modules |

Base exe (always resident): `.main`, rom 0x1050, VRAM `0x80000450`–`0x8007E020`,
plus its BSS/statics continuing up to `0x801CB460`.

**Decisive attribution test.** Tagging every reference by the rom-offset suffix
in its function name and matching against the section table:

- references to `0x8020EED0`: **499 refs across 378 distinct functions, ALL in file_13**, plus 1 in file_14. Zero elsewhere.
- references to `0x801FC604` (`g_player_1_task`): 85 in file_11, 22 in file_12, 8 in base exe, 174 in slot-C actors. **Zero in file_13.**

file_11 and file_13 occupy the same VRAM and never share state: file_13 is a
**self-contained alternate mode** with its own 378-function state machine (most
likely the Impact giant-robot battles — *not statically confirmed*). Its
counters are mode-local and are reset to 400/200 on area transition by
`func_80021444_22044`, so they are **not** ryo or health.

**Therefore any resource that survives a mode switch cannot live in
`0x801CB460`–`0x8020EED0`.** It must be in base-exe statics below `0x801CB460`
— which is exactly where the block in §2 was found. This constraint is what
made the search tractable, and it also means a lock cheat needs **no overlay
validity guard**.

Tooling left in place for follow-up work (in `/tmp/g64/`, regenerate if the
tmpdir is cleared): `flat.txt` (flat disassembly index, `0xVRAM insn | func`),
`an.py LO HI` (static-access map by effective address, via lui/addiu base
tracking), `owner.py <needle>` (attribute referencing functions to overlay
files), `cand.py LO HI` (rank always-resident cross-overlay integer globals).

---

## 2. The block, proven by the new-game initialiser

`func_8000B640_C240` — **base exe**, `$a0 = 0x8016<<16 - 0x39F8 = 0x8015C608`:

```
0x8000B648 lui   $a0, 0x8016
0x8000B64C addiu $a0, $a0, -0x39F8      ; a0 = 0x8015C608  (save buffer)
0x8000B650 jal   0x800041E0             ; bzero(...)
0x8000B654 addiu $a1, $zero, 0x304      ;   ... length 0x304   <- save buffer SIZE
0x8000B664 addiu $v1, $zero, 0xA        ; 10
0x8000B668 addiu $t6, $zero, 0x3        ; 3
0x8000B66C addiu $t7, $zero, 0x64       ; 100
0x8000B688 sw    $v0, 0x64($a0)         ; +0x64 = 1
0x8000B68C sw    $t6, 0x78($a0)         ; +0x78 = 3    -> LIVES
0x8000B690 sw    $v1, 0x70($a0)         ; +0x70 = 10   -> CURRENT HP
0x8000B694 sw    $v1, 0x6C($a0)         ; +0x6C = 10   -> MAX HP
0x8000B698 sw    $t7, 0x74($a0)         ; +0x74 = 100  -> MONEY
0x8000B6C4 addiu $a1, $a1, -0x36F0      ; a1 = 0x8015C910 (shadow copy)
0x8000B6C8 jal   0x80004208             ; copy(src=a0, dst=a1, len)
0x8000B6CC addiu $a2, $zero, 0x304
```

Four resources initialised side by side with textbook starting values
(10/10/100/3). This single function establishes both the save-buffer base and
the field order.

`func_8000B718_C318` — the live↔save mirror, and a lives floor:

```
0x8000B764 addiu $a1, $a1, -0x3994      ; a1 = 0x8015C66C  (save + 0x64)
0x8000B768 addiu $a0, $a0, -0x3A28      ; a0 = 0x8015C5D8  (LIVE block)
0x8000B76C addiu $a2, $zero, 0x30       ; length 0x30      <- LIVE block SIZE
0x8000B784 jal   0x80004208             ; copy
...
0x8000B794 lw    $t5, 0x78($a3)         ; a3 = 0x8015C608 ; lives
0x8000B7A0 slti  $at, $t5, 0x3
0x8000B7AC addiu $t6, $zero, 0x3        ; if (lives < 3) lives = 3
```

`live + 0x94 == save`, block length 0x30. Verified first-hand.

---

## 3. MONEY — `*(s32*)0x8015C5E8`, cap 9999 · confidence HIGH

### Writer: `func_801DCCF0_598C00` = `s32 change_money(s16 delta)` (file_11)

```
0x801DCCF0 lui   $v1, 0x8016
0x801DCCF4 addiu $v1, $v1, -0x3A28      ; v1 = 0x8015C5D8 (live block)
0x801DCCF8 lw    $t8, 0x10($v1)         ; money  = *(0x8015C5E8)
0x801DCCFC sll   $t6, $a0, 16
0x801DCD00 sra   $t7, $t6, 16           ; delta sign-extended from 16 bits
0x801DCD04 addu  $t9, $t8, $t7
0x801DCD08 sw    $t9, 0x10($v1)         ; money += delta          [WRITER]
0x801DCD0C lh    $v0, 0x12($v1)         ; low signed halfword of money
0x801DCD14 bgez  $v0, L_801DCD28
0x801DCD1C sw    $zero, 0x10($v1)       ; underflow -> money = 0, return 1
0x801DCD24 addiu $v0, $zero, 0x1
L_801DCD28:
0x801DCD18 slti  $at, $v0, 0x2710       ; >= 10000 ?
0x801DCD28 bne   $at, $zero, L_801DCD3C
0x801DCD2C addiu $t0, $zero, 0x270F
0x801DCD30 sw    $t0, 0x10($v1)         ; clamp -> money = 9999, return 1
0x801DCD3C or    $v0, $zero, $zero      ; in range -> return 0
```

Add-with-clamp on both ends. **Cap = 9999 (`0x270F`), floor = 0**, return value
= "clamped" flag. This is the sole general-purpose money mutator; both pickups
and purchases funnel through it with positive/negative deltas.

Note the quirk: the range test uses `lh` on the **low halfword** (`+0x12`), so
it only inspects the low 16 bits of the s32. Writing a value ≥ 0x10000 whose
low halfword happens to look small would slip past the clamp until the next
call — relevant to §7.

### Independent witnesses

1. **Field HUD `func_801FBD50_5B7C60`** (file_11) draws it as a **4-digit
   decimal number** with leading-zero suppression — the defining trait of a
   money counter. Base held in `$a0`:
   ```
   0x801FBE50 addiu $a0, $a0, -0x3A28   ; $a0 = 0x8015C5D8
   0x801FBF80 lw    $t4, 0x10($a0)      ; money = 0x8015C5E8
   0x801FBF84 slti  $at, $t4, 0x2710    ; SIGNED compare vs 10000
   0x801FBFC8 div   $zero, $t4, $at     ; $at=0x3E8 -> thousands
   0x801FC03C lw    $t8, 0x10($a0)      ; $at=0x64  -> hundreds
   0x801FC0B8 lw    $t6, 0x10($a0)      ; /100 %10  -> tens
   0x801FC11C lw    $t7, 0x10($a0)      ; /10  %10  -> units
   ```
   If `money >= 10000` it branches to `0x801FBF90`–`0x801FBFC0` and draws a
   fixed placeholder glyph four times. **It never stores** — the HUD does not
   write back, so an out-of-range value is a display artefact only, not
   corruption. (Contrast with the unrelated file_13 mode HUD, which *does*
   force its own counters to 0 — see §9.)
2. **Shop / purchase reader `func_801E4624_5A0534`** (file_11) — read-only
   affordability gate, repeated three times (`0x801E47C4`, `0x801E4820`,
   `0x801E487C`):
   ```
   0x801E47C4 lw    $t9, 0x10($t0)          ; money
   0x801E47CC slti  $at, $t9, 0x3           ; price = 3 ryo
   0x801E47D0 bne   $at, $zero, L_801E47E8  ; too poor -> message 0x7C
   0x801E47D8 jal   0x801DACDC              ; else     -> message 0x7F
   ```
   Compare-price / branch-if-insufficient: spendable currency, not a score.
3. **New-game init** (§2): `sw 100 -> save+0x74`, i.e. `0x8015C67C`, the mirror
   of `0x8015C5E8`. Starting purse of 100 ryo.
4. **Six spend call sites**, all passing negative deltas to `change_money`
   (all file_11):

   | caller | call site | delta |
   |---|---|---|
   | `func_801F0408_5AC318` | `0x801F0598` | `-3` |
   | `func_801EF87C_5AB78C` | `0x801EF9C8` | `-1` |
   | `func_801E8F50_5A4E60` | `0x801E9060` | `lh $a0, 0x3A($sp)` (variable) |
   | `func_801EBF48_5A7E58` | `0x801EBFAC` | `-1` |
   | `func_801E75D4_5A34E4` | `0x801E7638` | `-0xA`, right after dialogue `0x801DACDC` msg `0x82` — a purchase |
   | `func_801EE980_5AA890` | `0x801EEAE0` | `-1` |

### Every writer of money

| function | overlay | store | role |
|---|---|---|---|
| `func_801DCCF0_598C00` | file_11 | `0x801DCD08`, `0x801DCD1C`, `0x801DCD30` | **canonical add/spend + clamp** |
| `func_80221FD4_5DD4A4` | file_12 | `0x80222008`, `0x80222020` | same clamp logic, **bypasses the mutator**; delta from `*(s16*)0x801C7742`, status to `0x801C7750` (0=ok, 1=underflow, 2=capped). Probable script/pickup "give ryo" path |
| `func_08000AD4_7068C4` | file_47 (slot C) | `0x08000CC8` | whole-block initialiser |
| `func_08001954_6F1B64` | file_42 (slot C) | `0x08001B10` | whole-block initialiser |
| `func_08001BC4_71F394` | file_60 (slot C) | `0x08001CD4` | whole-block initialiser |
| `func_8000B640_C240` | base exe | `0x8000B698` | new-game default (writes mirror `0x8015C67C`) |
| `func_8000B718_C318` | base exe | bcopy | commits live block → mirror |

The `0x80004208` copy direction is proven `bcopy(src=$a0, dst=$a1, n=$a2)` from
`0x80004214 lbu $t6, 0x0($a0)` / `0x80004224 sb $t6, -0x1($a1)`; the call in
`func_8000B718_C318` has `$a0=0x8015C5D8, $a1=0x8015C66C, $a2=0x30` — i.e.
**live → save**.

Ruled out as static-analysis false positives (stale `lui` base tracking; all are
really `0x20($v0)` on a reloaded pointer): `sh@0x801FC464`, `sh@0x801FC46C`
in the HUD, and `swc1@0x801CBE80` in `func_801CBD88_587C98`.

---

## 4. HEALTH — current `*(s32*)0x8015C5E4`, max `*(s32*)0x8015C5E0` · confidence HIGH

### Writer: `func_801DCD48_598C58` = `s32 change_health(s8 delta)` (file_11)

Directly adjacent to `change_money` — the two are siblings in the same
overlay, which is itself corroborating. `$v1 = 0x8015C5D8`, so `+0x8` = max,
`+0xC` = current, `+0xF` = current's low signed byte.

```
0x801DCD6C lw    $a0, 0x8($v1)      ; MAX      = *(0x8015C5E0)
0x801DCD70 lb    $t7, 0xF($v1)      ; CURRENT (low signed byte)
0x801DCD78 slt   $at, $t7, $a0      ; current < max ?
0x801DCD88 sw    $a0, 0xC($v1)      ; else CURRENT = MAX        [heal clamp]
0x801DCDB0 addu  $t9, $t8, $a1
0x801DCDB4 sw    $t9, 0xC($v1)      ; CURRENT += delta
0x801DCDBC slt   $at, $a0, $t0
0x801DCDCC sw    $a0, 0xC($v1)      ; clamp CURRENT = MAX
; --- damage path (delta <= 0) ---
0x801DCDD8 lw    $t1, 0xC($v1)
0x801DCDDC addu  $t2, $t1, $a1      ; CURRENT += (negative)
0x801DCDE0 sw    $t2, 0xC($v1)
0x801DCDE4 lb    $t3, 0xF($v1)
0x801DCDE8 bgezl $t3, ...           ; still >= 0 -> alive, return 0
0x801DCDF0 sw    $zero, 0xC($v1)    ; clamp at ZERO, return 1 (dead)
```

Load-max / add / clamp-to-max / clamp-to-zero / death-return — the canonical
shape. Heal callers pass +8 and +20 (`func_801E5274_5A1184` @ `0x801E52DC`,
`0x801E52EC`).

### Independent witnesses

1. **Max-HP upgrade `func_8000B824_C424`** (base exe) — confirms both addresses
   *and which is which*:
   ```
   0x8000B82C lw    $v0, 0xFC($v1)   ; v1 = 0x8015C608 ; +0xFC = upgrade token count
   0x8000B838 slti  $at, $v0, 0x4
   0x8000B83C bne   $at, $zero, ret  ; need >= 4
   0x8000B844 lw    $t7, 0x8($a0)    ; a0 = 0x8015C5D8 -> MAX
   0x8000B84C sw    $t6, 0xFC($v1)   ; spend 4
   0x8000B850 addiu $t8, $t7, 0x2    ; MAX + 2
   0x8000B854 sw    $t8, 0x8($a0)    ; MAX     = 0x8015C5E0
   0x8000B858 sw    $t8, 0xC($a0)    ; CURRENT = 0x8015C5E4  (full heal)
   ```
   Only MAX is permanently incremented and never decremented — the signature of
   a persistent upgrade. (+2 max per 4 tokens.)
2. **Full-heal helper `func_8000B5BC_C1BC`** (base exe) — the cleanest possible
   statement of direction:
   ```
   0x8000B5C4 lw $t6, 0x8($v0)   ; v0 = 0x8015C5D8 : MAX
   0x8000B5CC sw $t6, 0xC($v0)   ; CURRENT = MAX
   ```
3. **HUD life meter `func_801FBD50_5B7C60`** (file_11) reads the low halfword of
   each word (big-endian): `lhu 0xE($a0)` = `0x8015C5E6` (CURRENT) and
   `lhu 0xA($a0)` = `0x8015C5E2` (MAX), each `slti 0x28` display-clamped at 40,
   then `sll $t6, $t7, 3` → `(value/2)*8` pixels.

### Units
**2 health units = 1 displayed bar segment.** Start 10 = 5 segments; the HUD
display clamp of 40 = 20 segments. Heal pickups are +8 and +20 (4 and 10
segments).

### All health write sites

| Function | Overlay | Store addresses | Effect |
|---|---|---|---|
| `func_801DCD48_598C58` | file_11 | `0x801DCD88`, `0x801DCDB4`, `0x801DCDCC`, `0x801DCDE0`, `0x801DCDF0` | damage/heal entry point |
| `func_8000B51C_C11C` | base exe | `0x8000B53C` | `current -= 1` drain tick, then death chain |
| `func_8000B5BC_C1BC` | base exe | `0x8000B5CC` | full heal |
| `func_8000B824_C424` | base exe | `0x8000B854`, `0x8000B858` | max +2 and full heal |
| `func_8000B640_C240` | base exe | `0x8000B690`, `0x8000B694` | new-game init (save copy) |

---

## 5. LIVES — `*(s32*)0x8015C5EC` · confidence HIGH

### Writer: `func_8000B578_C178` = "lose a life" (base exe)

```
0x8000B578 lui   $v0, 0x8016
0x8000B57C addiu $v0, $v0, -0x3A28   ; v0 = 0x8015C5D8
0x8000B580 lw    $t6, 0x14($v0)      ; lives = *(0x8015C5EC)
0x8000B58C addiu $t7, $t6, -0x1
0x8000B590 beq   $t7, $zero, L_8000B5A8
0x8000B594 sw    $t7, 0x14($v0)      ; lives-- (delay slot: always stores)  [WRITER]
0x8000B598 jal   0x8000B5BC          ; refill HP to MAX
0x8000B5A4 addiu $v0, $zero, 0x1     ; return 1 = continue
L_8000B5A8:
0x8000B5A8 or    $v0, $zero, $zero   ; return 0 = GAME OVER
```

### Death chain (all small base-exe leaves)

`func_8000B4DC_C0DC` (top-level entry) → `func_8000B51C_C11C` (`HP--`; at 0
checks the revive item via `func_801E5274_5A1184`) → `func_8000B578_C178`
(lose a life) → `func_8000B5BC_C1BC` (refill HP).

### Independent witnesses
1. New-game init: `sw 3 -> save+0x78` = `0x8015C680`, the mirror of `0x8015C5EC` (§2).
2. Restore/continue floor in `func_8000B718_C318`: `if (save+0x78 < 3) = 3` @ `0x8000B7B0`.
3. HUD `func_801FBD50_5B7C60` reads `0x8015C5EC` @ `0x801FBF44`.

---

## 6. Secondary / not fully identified

| Address | Save off | Evidence | Confidence |
|---|---|---|---|
| `0x8015C5F0` | +0x7C | revive item (consulted by `func_801E5274_5A1184` when HP hits 0 before a life is spent); init 0 | MEDIUM |
| `0x8015C704` | +0xFC | max-HP upgrade token, consumed 4-at-a-time by `func_8000B824_C424`. In-game identity (Fortune Doll / Lucky Cat) is an **inference, not proven** | MEDIUM |
| `0x8015C5DC` | +0x68 | `g_current_player_character_id` (already named in `patches/variables.h`) | HIGH |
| `0x8015CD08` | — | 173 accesses, all `lbu`/`sb`, from dozens of slot-C actor modules — almost certainly the **event/item flag array**. Not decoded | LOW (identity), HIGH (that it is a flag array) |
| save `+0x00`–`+0x63`, `+0x220`–`+0x303` | — | flag/inventory regions, semantics undecoded | — |

Resources are **global, not per-character**: one 0x30-byte live block, and the
HP/money/lives slots are not indexed by `g_current_player_character_id`.

### Save I/O (context for persistence claims)
Controller Pak, **not** SRAM — the ROM links the `osPfs*` file API
(`osPfsInit` 0x800473D0, `osPfsFindFile` 0x80044CB0, `osPfsAllocateFile`
0x80044500, `osPfsReadWriteFile` 0x8004525C, `osPfsChecker` 0x8004D2E0).
Base-exe wrappers cluster at `0x80023394`–`0x800236F8`; the save-menu overlay is
file_23 (`func_801CB578_6A9A38`, `func_801CB870_6A9D30`, `func_801CB8F4_6A9DB4`,
`func_801CB9C8_6A9E88`). Save routine reported as `func_80214D58_5D0228`, CRC-32
at `func_80023A1C_2461C`, game code "NG5E"/"A4". *I did not personally verify the
CRC routine or the game code string — treat those two as MEDIUM confidence.*

---

## 7. Recommended lock implementations

All four values are **plain fixed-address base-exe statics**, always resident.
That is the ideal case: **no `RECOMP_PATCH` of any writer is required**, and no
overlay validity guard is needed.

### Preferred: a per-frame guest-side write from the existing patched hook

`patches/main.c` already patches the per-frame function
`func_800012FC_1EFC`, and already calls `update_analog_camera()` from it. Add an
`update_cheats()` beside it. Guest patch code dereferences raw absolute
addresses directly — this is the established idiom in `patches/camera.c`
(`*(u32*)0x8020CA2C`, `*(u8*)(*(u32*)0x801FC624 + 0xAB) = 0;`).

```c
/* patches/cheats.c (new file) */
#define G_MAX_HP   (*(volatile s32*)0x8015C5E0)
#define G_CUR_HP   (*(volatile s32*)0x8015C5E4)
#define G_MONEY    (*(volatile s32*)0x8015C5E8)
#define G_LIVES    (*(volatile s32*)0x8015C5EC)

void update_cheats(void) {
    if (!in_gameplay()) return;              /* see gating note below */
    if (recomp_get_infinite_health_mode()) G_CUR_HP = G_MAX_HP;  /* mirrors func_8000B5BC_C1BC */
    if (recomp_get_infinite_money_mode())  G_MONEY  = 9999;      /* 0x270F — the game's own cap */
    if (recomp_get_infinite_lives_mode())  G_LIVES  = 3;
}
```

Wiring needed: a `recomp_get_infinite_health_mode` / `..._money_mode` dummy
address in `patches/syms.ld` (next to the existing `recomp_get_*` entries), the
matching `extern "C"` shim in `src/game/recomp_api.cpp` returning
`goemon64::get_infinite_health_mode()`, and a declaration in
`patches/patch_api_funcs.h`. The config getters already exist.

**Per-resource notes:**

- **Health** — write `G_CUR_HP = G_MAX_HP` rather than a constant. This is
  byte-for-byte what the game's own `func_8000B5BC_C1BC` does, so it cannot
  desync the HUD (which reads the low halfword) or the heal clamp. Do **not**
  write a large constant: the HUD clamps display at 40 but nothing clamps the
  variable, and `change_health` reads current as a **signed byte** (`lb 0xF`),
  so any value whose low byte is ≥ 0x80 reads as negative and would register as
  *death*. This is a real footgun — keep current ≤ max and max small.
- **Money** — lock to **9999 (`0x270F`), never higher**. `change_money`'s range
  check is `lh $v0, 0x12($v1)` — big-endian, so it inspects only the **low
  signed halfword**. Any locked value whose low halfword has bit 15 set
  (`0x7FFFFFFF`, or anything ≥ 0x8000) reads as negative, and the next
  `change_money` call takes the underflow branch and **zeroes your money**.
  9999 is the safe, game-legal maximum. Purchases still work: the shop
  subtracts, and the next frame restores.
  Also write the save mirror `*(s32*)0x8015C67C = 9999;` so a commit can't roll
  it back. **Do not** implement this by patching `func_801DCCF0_598C00`: that
  only blocks spending, and the file_12 writer `func_80221FD4_5DD4A4` bypasses
  the mutator entirely. A per-frame write covers every path.
- **Lives** — a per-frame write of 3 is safe and simple. The alternative, if you
  want the death animation to behave exactly as the developers intended, is a
  `RECOMP_PATCH` of `func_8000B578_C178` that returns 1 without decrementing but
  still calls `func_8000B5BC_C1BC` (the HP refill). That preserves the whole
  death→refill sequence and is arguably cleaner, at the cost of an overlay-free
  but function-level patch.

### Persistence caveat
Writing only the live block is correct for gameplay. The live block is copied to
the save buffer (`live + 0x94`) by `func_8000B718_C318` at area transitions, so
locked values propagate into the save file naturally — **the cheat will
permanently alter the player's Controller Pak save.** If that is undesirable,
either warn the user in the UI or additionally restore the save mirror. Do not
write the save mirror directly instead of the live block; gameplay reads only
the live copy.

### Gating
`func_800012FC_1EFC` runs every frame including menus and non-gameplay modes.
Gate the writes. The cheapest reliable gate is the one `patches/camera.c`
already uses (`acam_in_gameplay()`, a pointer-sanity check on `*0x8020CA2C`);
alternatively gate on file_11 being the resident slot-A overlay. **Un-gated
writes during the file_13 alternate mode would be harmless to that mode (it does
not touch this block) but could corrupt menu/save-screen state — gate anyway.**

Gating matters for a second reason: the block is **zeroed and re-initialised**
during boot and new-game by `func_8000B640_C240` and by three slot-C
initialisers (`func_08000AD4_7068C4`, `func_08001954_6F1B64`,
`func_08001BC4_71F394`). An un-gated lock racing those would fight the
initialiser and could leave the save in an odd state on a fresh file.

---

## 8. What could NOT be confirmed statically

1. **Everything here is unverified on hardware.** No value was observed live. On-device confirmation of all four addresses is the single most valuable next step (drop a temporary `recomp_printf` of the four words into the per-frame hook and watch them while taking damage, collecting ryo, and dying).
2. **MAX vs CURRENT health labelling** rests on clamp *direction* (`func_8000B5BC_C1BC` copies `+0x8` → `+0xC`) and on the upgrade path, not on a labelled HUD draw. The two are adjacent and both init to 10, so a swap would be easy to miss — worth 30 seconds of on-device checking.
3. **The true ceiling of MAX health.** 40 is the *HUD display clamp* only; no writer enforces it on the variable. Whether the game is stable above 40 is unknown.
4. **Which concrete game mode file_13 is.** "Impact battles" is a plausible guess, not proven. This does not affect any address above.
5. **Money cap behaviour above 0x10000** (the low-halfword `lh` quirk) is inferred from the instruction encoding, not observed.
6. **The identity of the `+0xFC` upgrade token** and of the revive item at `0x8015C5F0`.
7. **The save-buffer flag regions** (`+0x00`–`+0x63`, `+0x220`–`+0x303`) are undecoded — a "unlock everything" cheat would need those mapped.
8. Whether the drain tick `func_8000B51C_C11C` is drowning, lava, or a timer — the `-1` and the death chain are certain, the trigger is not.
9. **The money PICKUP path.** Every direct call site of `change_money` passes a *negative* delta (spending). `func_80221FD4_5DD4A4` (file_12) is the natural "give ryo" entry, but it has **zero direct `jal` call sites** — it is reached via a pointer or an overlay entry table — and its delta source `*(s16*)0x801C7742` has no static writer, being written through a computed pointer. So the coin-pickup path is inferred, not proven. Confidence MEDIUM. (Does not affect the address or the cap.)
10. **Writer-list completeness.** The static base-tracking used here resolves `lui`/`addiu`-formed addresses only. Code that reaches the block through a **passed-in pointer** would be missed — and this demonstrably happens (the HUD receives the block base in `$a0`). The writer tables should be read as "complete for direct static access", confidence MEDIUM for absolute completeness.
11. The **save → live restore** direction. Only the commit direction (live → save) was proven.
12. `lh $a0, 0x3A($sp)` at `0x801E9064` — a variable money delta; a variable price or a coin value. Unresolved.

---

## 9. Corrections to earlier notes

- **The earlier note is CORRECT** on `0x801FC604`: `func_801CB5D0_5874E0` does
  `lui $s0, 0x8020` / `addiu $s0, $s0, -0x39FC` = `0x80200000 - 0x39FC =
  **0x801FC604**` (verified at `0x801CB670`–`0x801CB680`). An intermediate
  analysis in this sweep reported `0x8020C604`; that was an arithmetic slip and
  is **wrong**. Same for `0x801FC60C`.
- `patches/variables.h` names `0x801FC604` as `PlayerTask *g_player_1_task`
  (not a generic "control object") and `0x801FC608` as `g_player_2_task`;
  `PlayerTask+0x90` is `player_id`, not a "movement-mode byte"; `+0xC0/C4/C8`
  are unnamed floats. Prefer `patches/types.h` / `patches/variables.h` over the
  older prose in the earlier notes where they disagree.
- `func_801CDD10_5F90F0` (file_13) is **both** things: it opens as a state
  machine dispatching on `+0x2E` through a jump table at `0x8020D000`-ish, and
  one of its branches (`0x801CE690`–`0x801CE8F8`, read directly) renders three
  counters off `*(u32*)0x8020EED0` as decimal digits — `+0x60` as 4 digits
  (zeroed if ≥ `0x2710`), `+0x64` and `+0x68` as 3 digits (zeroed if ≥ `0x3E8`),
  via draw helper `0x8000E23C`. Unlike the field HUD, this one **does force its
  counter to 0** when out of range. But these are **file_13 mode-local**
  counters, reset to 400/200 on area transition by `func_80021444_22044` — they
  are not ryo or health. The real field HUD (life meter + 4-digit money) is
  `func_801FBD50_5B7C60` in file_11.
- The `*(u32*)0x8020EED0` object is **mode-local to file_13** and is not global
  game state. Its counters are not ryo/health. Do not build cheats on it.
