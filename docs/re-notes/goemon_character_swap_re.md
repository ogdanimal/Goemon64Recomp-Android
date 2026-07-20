# Goemon64 — Player character swapping: the variable, the routine, the gate

Static reverse-engineering sweep over the recompiler output
(`RecompiledFuncs/funcs_*.c`) via the flat disassembly index (`/tmp/g64/flat.txt`,
516,976 instructions) plus `/tmp/g64/an.py` (static effective-address map) and the
section table in `Goemon64RecompSyms/mnsg.syms.toml`. No emulator, no on-device
testing — every claim below is backed by quoted disassembly. Date: 2026-07-18.

Motivation: the user wants character swapping **on the fly** (while moving /
mid-action) instead of only in the game's restricted circumstances.

---

## TL;DR — the answers

| Question | Answer |
|---|---|
| **Character id variable** | `0x8015C5DC`, `u32` (`lw`/`sw`), range 0..3. Also mirrored per-task in `PlayerTask.character_id` (`+0x60`, `u8`). |
| **Swap trigger input** | **C-Down HELD** (`buttons_held & 0x4`), read from the processed controller record `0x800C7DB0 + 24*player_id`, field `+0x2`. |
| **Swap entry point** | `func_801DCE10_598D20` (per-frame C-button dispatcher) → `func_801DD3C4_5992D4` (**THE GATE**) → `func_801DD50C_59941C` (pick next character) → `func_801DD5C0_5994D0` (commit) |
| **THE GATE** | `func_801DD3C4_5992D4` @ `0x801DD3C4`. Primary condition: **`PlayerTask.action_id` (`+0xCC`) must be 0, 1, or 0x42** — the three *idle stances*. Anything else (walking, jumping, attacking, damaged, swimming, …) returns 0. |
| **How heavy is the swap?** | **VERY heavy.** It DMAs + decompresses a whole character graphics file from ROM into a fixed 0x18000-byte per-player buffer, then re-uploads/relocates every texture. It runs as a ~30+ frame state machine with player input hard-locked. It is **not** a re-point of already-loaded data. |
| **Feasibility verdict** | Relaxing the gate to "any grounded locomotion state" is plausible and low-risk. Relaxing it to *truly* any state (mid-air, mid-attack, on a ladder/rope, riding) is **not** safe — see §7. Recommended: `RECOMP_PATCH` of `func_801DD3C4_5992D4`. |

---

## 1. The character id variable — PROVEN

**`0x8015C5DC`, `u32`.** Already named `g_current_player_character_id` in
`patches/variables.h:45` (`extern u32 D_8015C5DC_15D1DC;`).

It sits at **save-buffer offset `+0x68`** (save base `0x8015C608`, live block base
`0x8015C5D8`; see `goemon_cheats_re.md` §2 for the block layout).

`an.py 0x8015C5D8 0x8015C5E0` gives the complete access map — 30 accesses, 11 stores:

**Writers (the ones that matter):**

| Address | Function | Overlay | What it is |
|---|---|---|---|
| `0x801DD5DC` | `func_801DD5C0_5994D0` | file_11 | **the swap commit** (§3) |
| `0x801DD670` | `func_801DD654_599564` | file_11 | alternate swap commit — **no callers**, dead code (§3.4) |
| `0x800214B0` | `func_80021444_22044` | base exe | area-transition reset |
| `0x8005227C` | `func_80037414_38014` | base exe | init |
| `0x08000CBC`, `0x080018E0`, `0x08002C44` | `func_08000AD4_7068C4`, `func_08001954_6F1B64` | slot C | scripted/story forced character set |
| `0x080024A8` | `func_08001D2C_71F4FC` | slot C | scripted forced character set |

**Readers:** `func_801CB5D0_5874E0` (spawn — picks which model to load),
`func_801FBD50_5B7C60` (HUD, 7 reads — per-character HUD portrait/gauge),
`func_8001FA80_20680`, `func_80020440_21040`, `func_800207AC_213AC` (base exe),
`func_802182CC_681CAC` (5 reads, file_18 = pause/party menu),
`func_080014E8_72CD38` (slot C).

**Value range 0..3** — proven two independent ways:
1. `func_801DD50C_59941C` @ `0x801DD538`: `slti $at, $v1, 0x4` — the candidate
   wraps to 0 whenever `id+n >= 4`.
2. `patches/variables.h:41`: `extern u16 D_80204020_5BFF30[4]; // g_character_graphics_file_ids`
   — a 4-entry table indexed by the id (dereferenced at `0x801DC9E4`).

Identity of each index (Goemon / Ebisumaru / Sasuke / Yae) is **INFERRED** from
`func_8000B640_C240` seeding availability slots 0 and 1 at new game (Goemon +
Ebisumaru are the starting pair in MNSG) — the exact 2↔3 assignment is **not
statically proven** and needs one on-device check.

**Second, per-task copy: `PlayerTask.character_id` at `+0x60` (`u8`)** — already
named in `patches/types.h:223`. Written in lockstep with the global by both
commit functions (`sb $a2, 0x60($a0)` @ `0x801DD5D0` / `0x801DD664`).

### 1.1 Which characters are *available* — `0x8015C69C[4]` (save `+0x94`)

`func_801DD50C_59941C` loops over `*(u32*)(0x8015C608 + 0x94 + 4*id)`:

```
0x801DD528 addiu $a0, $a0, -0x39F8   ; a0 = 0x8015C608   (SAVE buffer base)
0x801DD550 sll   $t6, $v0, 2
0x801DD554 addu  $t7, $a0, $t6
0x801DD558 lw    $t8, 0x94($t7)      ; = 0x8015C69C + 4*id
0x801DD55C beq   $t8, $zero, ...     ; 0 -> character not in the party, skip
```

Corroborated by `an.py 0x8015C69C 0x8015C6AC`: new-game init
`func_8000B640_C240` stores to `+0x0` and `+0x4` (`0x8000B6A0`, `0x8000B6A4`);
the story actor `func_08001BC4_71F394` stores to all four (party recruitment);
the party menu `func_80213988_672938` reads three of them. This table is read
**directly out of the save buffer**, not from a live mirror.

---

## 2. The input path — PROVEN

### 2.1 Button record layout

`func_80004AF8_56F8` (base exe) reads the raw `OSContPad` (6-byte stride, `s5=6`,
array at `0x801671A0`) and derives the processed record:

```
0x80004CC4 lhu $v0, 0x0($a0)     ; v0   = raw OSContPad.button (u16, verbatim N64 bits)
0x80004CC8 lhu $t4, 0x2($s0)     ; prev = previous frame's held mask
0x80004CCC sh  $v0, 0x2($s0)     ; rec+0x2 = HELD
0x80004CD0 xor $t5, $t4, $v0
0x80004CD4 and $t6, $t5, $v0
0x80004CD8 sh  $t6, 0x4($s0)     ; rec+0x4 = PRESSED (rising edge)
```

`s0 = 0x800C7D38 + 24*i` (raw-stage record), copied wholesale to
`s1 = 0x800C7DB0 + 24*i` (the record CLAUDE.md already documents: `+0xC`
magnitude, `+0x10`/`+0x14` planar stick) at `0x80004F34`–`0x80004F64`, **unless**
`*(u8*)0x800C7A23` or `*(u8*)0x800C7A24` is set, in which case the record decays
toward neutral instead (scripted input lock).

Because the bits are copied verbatim from `OSContPad.button`, the standard N64
mask applies: `0x0010`=R, `0x0008`=C-Up, `0x0004`=C-Down, `0x0002`=C-Left,
`0x0001`=C-Right.

### 2.2 The C-button dispatcher — `func_801DCE10_598D20` @ `0x801DCE10`

Called once per frame from the player update `func_801CB824_587734` @ `0x801CB98C`.
Decoded:

```c
u32 handle_c_buttons(PlayerTask *task) {          // a0
    u8  *rec  = (u8*)(0x800C7DB0 + 24 * task->player_id);   // player_id = task+0x90
    u16 held  = *(u16*)(rec + 0x2);               // 0x801DCE40
    u16 press;
    u32 v0 = 0;

    if (held & 0x10) return 0;                    // 0x801DCE4C/50  — R held: abort
    if (*(u8*)0x800C7AE0 & 0x4) return 0;         // 0x801DCE58/64  — scripted "no C-actions" flag

    press = *(u16*)(rec + 0x4);                   // 0x801DCE74
    if      (press & 0x8) v0 = 2;                 // C-Up    (edge)
    else if (press & 0x2) v0 = 3;                 // C-Left  (edge)
    else if (held  & 0x4) v0 = 4;                 // C-Down  (LEVEL, not edge)

    switch (v0) {
    case 2: /* ... item/ability ... */
    case 3: /* ... */
    case 4:                                       // ===== CHARACTER SWAP =====
        if (!func_801DD3C4(task)) return 0;       // 0x801DCF64/6C  — THE GATE
        if (func_801DD50C(task) != 0) return 0;   // 0x801DCF74/7C  — pick+commit; !=0 = no partner
        func_801E8964(task, 3);                   // 0x801DCF84 — request animation/effect 3
        func_80038BC8(0x21A);                     // 0x801DCF8C — play swap SFX 0x21A
        return 4;
    }
    return 0;
}
```

Caller side (`func_801CB824_587734`):

```
0x801CB974 lbu  $v1, 0x7AE0($v1)      ; *(u8*)0x800C7AE0
0x801CB978 andi $t1, $v1, 0x1         ; bit0 -> return (all input locked)
0x801CB984 andi $t2, $v1, 0x2         ; bit1 -> return (all input locked)
0x801CB98C jal  0x801DCE10            ; the dispatcher
0x801CB994 addiu $at, $zero, 0x4
0x801CB998 bne  $v0, $at, ...         ; ==4 (swap started) -> func_801E8D90(task,1); return
```

`0x800C7AE0` is a global "player-control restriction" byte written by ~10 slot-C
actor modules (event/cutscene triggers) — `an.py 0x800C7AE0 0x800C7AE4`.
Bits 0x1/0x2 lock *all* input; bit 0x4 locks the C-button actions specifically.
This is the game's per-scene "you may not swap here" lever — it is **not** the
motion gate.

---

## 3. The swap routine

### 3.1 Pick the next available character — `func_801DD50C_59941C` @ `0x801DD50C`

```c
s32 swap_to_next(PlayerTask *task) {
    u8  cur = task->character_id;          // +0x60
    s32 pick = -1;
    for (s32 n = 1; n < 4; n++) {
        s32 c = (cur + n < 4) ? (cur + n) : 0;                // 0x801DD538
        if (*(u32*)(0x8015C69C + 4*c) != 0) { pick = c; break; }  // 0x801DD558
    }
    if (pick < 0) return 1;                                   // no partner -> caller returns 0
    *(u32*)0x8015C5DC = pick;                                 // 0x801DD594
    *(u32*)0x8015C604 = 1;                                    // 0x801DD598  ("swap pending" latch)
    task->character_id = pick;                                // 0x801DD588
    func_801DD5C0(task, pick);
    return 0;
}
```

Note the wrap is `-> 0`, not a true modulo, so from `cur=2` the probe order is
3, 0, 0 — harmless in practice but worth knowing.

### 3.2 Commit — `func_801DD5C0_5994D0` @ `0x801DD5C0`

```c
void commit_character(PlayerTask *task, u8 id) {
    task->character_id      = id;              // sb  a2, 0x60(a0)
    *(u32*)0x8015C5DC       = id;              // sw  t6, -0x3A24(0x8016<<16)
    task->player->0x69      = 1;               // sb  1,  0x69(player)   <- INPUT LOCK
    task->player->0x86      = 0;               // sh  0,  0x86(player)
    task->player->0x30      = 0;               // sh  0,  0x30(player)   <- sub-phase = 0
    task->player->0x00      = 1;               // sh  1,  0x0(player)
    task->unknown_80        = 0;               // sh  0,  0x80(a0)       <- frame counter
    task->0xE8 = task->0x68 = task->0x6C = task->0x70 = 0.0f;   // <- VELOCITY ZEROED
    task->0x48              = 0;
    task->0xDB              = 1;
    task->0x30             &= 0xFFFE;
    task->action_id         = 0xBA;            // sb  0xBA, 0xCC(a0)
    func_8003522C(task, 0x801E0944);           // install the swap state machine
}
```

The `player->0x69 = 1` write is the hard input lock: `func_801CB824_587734`
returns immediately at `0x801CB83C` while it is non-zero.

### 3.3 The swap state machine — `func_801E0944_59C854` @ `0x801E0944`

Action `0xBA`. Dispatches on `player->0x30` (6 sub-phases, jump table at
`0x8020B1D8`):

| Phase | Address | What it does |
|---|---|---|
| 0 | `0x801E0988` | wait 5 frames (`task+0x80`); zero the camera fade fields; snapshot `*(s16*)0x800C7A72` into a per-player slot |
| 1 | `0x801E09E4` | spin until `*(s16*)0x800C7A72` differs from the snapshot (screen fade / smoke puff completes) |
| 2 | `0x801E0A24` | **`func_801DC9C8(task, character_id)` — load the new character's graphics file from ROM** |
| 3 | `0x801E0A60` | call `func_801DCA64(task, 2, &counter)` once per frame, 2 textures at a time, until it returns 0 — **texture upload / relocation** |
| 4 | `0x801E0AA4` | `player->0x69 = 0` (unlock input); reinstall the animation; `action_id = 0xBA` again; `func_801DD830(task,3)`; `func_801CBFCC(task)` |
| 5 | `0x801E0B48` | wait `0x18` (24) frames, then `func_801CC454(task)`, `player->0x00 = 0`, and pick the terminal `action_id` |

**Terminal action selection (this is the key evidence for the gate):**

```
0x801E0B84 lbu  $t4, 0x59($t2)        ; player->0x59
0x801E0B88 addiu $t3, $zero, 0x42
0x801E0B98 sb   $t3, 0xCC($s0)        ; player->0x59 != 0  ->  action_id = 0x42
...
0x801E0BA4 lw   $t6, 0xC($v0)         ; v0 = 0x8015C5D8, +0xC = 0x8015C5E4 = CURRENT HP
0x801E0BA8 lw   $t5, 0x8($v0)         ;               +0x8 = 0x8015C5E0 = MAX HP
0x801E0BB0 sll  $t7, $t6, 2           ; cur * 4
0x801E0BB4 slt  $at, $t5, $t7         ; max < cur*4  ?
0x801E0BB8 bne  $at, $zero, L_801E0BDC ;   yes (healthy) -> action_id = 0
0x801E0BD0 sb   $t9, 0xCC($s0)        ;   no  (hurt)    -> action_id = 1
0x801E0BE0 sb   $zero, 0xCC($s0)      ;                    action_id = 0
```

So `{0, 1, 0x42}` = **normal idle**, **low-health idle** (HP ≤ ¼ max), and the
`player->0x59` special idle. These are exactly the three values the gate accepts.

**Cost of a swap:** 5 frames + fade + 1 + N texture frames + 1 + 24 frames.
Well over half a second, with input locked for all but the last ~24 frames.

### 3.4 `func_801DD654_599564` / `func_801E0BF8_59CB08` — dead code

`func_801DD654_599564` @ `0x801DD654` is a byte-for-byte sibling of
`func_801DD5C0_5994D0` that sets `action_id = 0xBB` and installs
`func_801E0BF8_59CB08` instead. **`grep 'jal +0x801DD654'` returns zero hits** —
it has no callers anywhere in the ROM. Presumably a cut second swap animation.
Not a usable entry point, but a ready-made second variant if one is ever wanted.

### 3.5 The graphics load — `func_801DC9C8_5988D8` @ `0x801DC9C8` — **this is the "heavy" part**

```c
u32 load_character_graphics(PlayerTask *task, u8 id) {
    u16 *tbl  = (u16*)0x80204020;                      // g_character_graphics_file_ids[4]
    void *node = *(void**)((u8*)task + 0x18);          // object transform node
    *(u16*)((u8*)node + 0x3C) = tbl[id];               // record the loaded file id
    void **bufs = (void**)0x8020D220;                  // g_player_graphics_buffers[2]
    u8 *end = func_80001C00_2800(tbl[id], bufs[task->player_id]);  // file_load: ROM DMA + decompress
    for (s32 i = 15; i != 0; i--) { }                  // 0x801DCA24 busy-wait
    if ((u8*)bufs[task->player_id] + 0x18000 < end) return 0;      // OVERFLOW CHECK
    return 1;
}
```

- `func_80001C00_2800` is already named `file_load` in `patches/functions.h:26`.
- `0x8020D220` is already named `g_player_graphics_buffers` in
  `patches/variables.h:43` — **two** buffers (2 players), `0x18000` bytes each.
- Corroborating witness: `func_801CBC40_587B50` — the *initial* character load on
  area entry — calls the exact same pair `func_801DC9C8` + `func_801DCA64`.
  So this is the model loader, not something swap-specific.

`func_801DCA64_598974` @ `0x801DCA64` then walks a texture descriptor list,
allocating/uploading through the texture cache (`0x80014698`, `0x800148F0`,
`0x80014B74`, `0x800144E8`) — incremental, `a1` textures per call.

**Consequence:** a swap **overwrites the single shared per-player graphics
buffer**. The old character's model and textures are destroyed in place. There
is no way to make this cheap; it cannot become a re-point of already-resident
data without a large engine change (double-buffering all four characters would
need 4 × 0x18000 = 384 KiB of a 4 MiB RDRAM budget).

---

## 4. THE GATE — `func_801DD3C4_5992D4` @ `0x801DD3C4` — PROVEN

Full function, decoded:

```c
u32 can_swap_character(PlayerTask *task) {          // a0
    u8 act = task->action_id;                       // 0x801DD3C4  lbu v0, 0xCC(a0)

    /* ---- CONDITION A: action-state whitelist ---- */
    if (!(act < 2) && act != 0x42) return 0;        // 0x801DD3C8 .. 0x801DD3DC

    /* ---- CONDITION B: the attached "carried/mounted" object, if any ---- */
    void *o = *(void**)((u8*)task + 0x38);          // 0x801DD3E0  lw v0, 0x38(a0)
    if (o != NULL && *(u8*)((u8*)o + 0x4C) != 2)    // 0x801DD3F8/0x801DD400
        return 0;

    /* ---- CONDITION C: camera not busy ---- */
    void *camtask = *(void**)0x801FC624;            // 0x801DD408  lui 0x8020 / lw -0x39DC
    if (*(u8*)((u8*)camtask + 0xD2) != 0) return 0; // 0x801DD40C/0x801DD410

    /* ---- CONDITION D: character-1 special case ---- */
    if (task->character_id == 1 &&                  // 0x801DD418/0x801DD420
        *(u16*)((u8*)task->player + 0x86) == 0xFFFF) // 0x801DD430/0x801DD434
        return 0;

    return 1;                                       // 0x801DD448
}
```

Exact instruction encoding of Condition A (the important one):

```
0x801DD3C4 lbu   $v0, 0xCC($a0)          ; action_id  (unsigned)
0x801DD3C8 bltz  $v0, L_801DD3D4         ; never taken (lbu is 0..255)
0x801DD3CC slti  $at, $v0, 0x2           ;   delay slot
0x801DD3D0 bne   $at, $zero, L_801DD3E0  ; action_id < 2   -> allowed
0x801DD3D4 addiu $at, $zero, 0x42        ;   delay slot
0x801DD3D8 bnel  $v0, $at, L_801DD450    ; action_id != 0x42 -> return 0
0x801DD3DC or    $v0, $zero, $zero       ;   delay slot: v0 = 0
```

### Why the game requires standing still — plain terms

The whitelist `{0, 1, 0x42}` is **not** "grounded" and **not** "velocity == 0".
It is literally *"you are currently playing one of the three idle-stance
animations"* — proven by §3.3: those three ids are exactly the states the swap
state machine itself terminates into, chosen by `player->0x59` and by the
HP-ratio test between `0x8015C5E0` (max HP) and `0x8015C5E4` (current HP).

The gate therefore enforces a **closed loop**: the swap starts from an idle
stance and ends in an idle stance. The swap sequence has no notion of resuming a
prior action — phase 4/5 unconditionally reinstall an idle animation and an idle
`action_id`. It also unconditionally zeroes `task->0x68/0x6C/0x70/0xE8`
(velocity) at commit time (`0x801DD614`–`0x801DD620`). Nothing in the routine
saves, restores, or even inspects the pre-swap action; it assumes it was idle.

Conditions B–D are **not motion gates** and are shared with the sibling C-button
actions (Condition C is byte-identical to the first four instructions of
`func_801DCFB0_598EC0`, the C-Up gate). They should be preserved.

### Additional gates upstream (not in `func_801DD3C4`)

| Where | Condition | Meaning |
|---|---|---|
| `func_801CB824_587734` @ `0x801CB83C` | `player->0x69 != 0` → return | a swap (or other lock) is already in progress |
| `func_801CB824_587734` @ `0x801CB978/84` | `*(u8*)0x800C7AE0 & 0x3` | scripted total input lock |
| `func_801DCE10_598D20` @ `0x801DCE50` | `held & 0x10` (R) | R held suppresses all C-button actions |
| `func_801DCE10_598D20` @ `0x801DCE64` | `*(u8*)0x800C7AE0 & 0x4` | scripted "no C-actions here" |
| `func_801DD50C_59941C` @ `0x801DD558` | `0x8015C69C[c] == 0` | that character isn't in the party |

---

## 5. Call graph summary

```
func_801CB824_587734            (per-frame player update, file_11)
  └─ func_801DCE10_598D20       (C-button dispatcher)          0x801DCE10
       └─ [C-Down held, case 4]
            ├─ func_801DD3C4_5992D4   THE GATE                 0x801DD3C4
            ├─ func_801DD50C_59941C   pick next available      0x801DD50C
            │    └─ func_801DD5C0_5994D0  commit               0x801DD5C0
            │         └─ func_8003522C(task, func_801E0944)
            ├─ func_801E8964_5A4874(task, 3)   animation/effect
            └─ func_80038BC8(0x21A)            SFX

func_801E0944_59C854            swap state machine (action 0xBA)  0x801E0944
  ├─ phase 2: func_801DC9C8_5988D8   ROM load of the model       0x801DC9C8
  │             └─ func_80001C00_2800  (file_load)
  └─ phase 3: func_801DCA64_598974   texture upload/relocate     0x801DCA64

(dead) func_801DD654_599564 -> func_801E0BF8_59CB08   action 0xBB, no callers
```

---

## 6. PROVEN vs INFERRED

### PROVEN (≥2 independent lines of evidence, addresses cited)

- `0x8015C5DC` is the current character id, `u32`, 0..3.
  *(a)* written by both commit functions alongside `PlayerTask.character_id`;
  *(b)* used to index the 4-entry `g_character_graphics_file_ids` table at
  `0x801DC9E4`; *(c)* already named in `patches/variables.h`.
- Swap input is **C-Down held**, mask `0x4` of `*(u16*)(0x800C7DB0 + 24*id + 0x2)`.
  *(a)* `func_80004AF8_56F8` copies `OSContPad.button` verbatim into `+0x2` and
  computes `+0x4` as the rising edge; *(b)* the record base matches CLAUDE.md's
  already-proven `0x800C7DB0 + 24*idx` stick record.
- `func_801DD3C4_5992D4` is the swap gate. *(a)* it is the sole predicate guarding
  the `func_801DD50C` call in case 4; *(b)* it has exactly one caller.
- The whitelist is `action_id ∈ {0, 1, 0x42}`. *(a)* the branch encoding at
  `0x801DD3C8`–`0x801DD3DC`; *(b)* those are precisely the three values the swap
  state machine writes back at `0x801E0B98` / `0x801E0BD0` / `0x801E0BE0`.
- The swap performs a full ROM file load. *(a)* `func_801DC9C8` calls
  `func_80001C00_2800` (`file_load`, named in `patches/functions.h:26`) with a
  0x18000-byte destination buffer; *(b)* the same function pair is used by
  `func_801CBC40_587B50`, the initial spawn-time model load.
- Velocity is zeroed at commit: `swc1 $f0, 0x68/0x6C/0x70/0xE8($a0)` @
  `0x801DD614`–`0x801DD620`, with `$f0 = 0.0` from `mtc1 $zero, $f0` @ `0x801DD5E8`.
- Input is locked during the swap via `player->0x69` (set 1 @ `0x801DD5EC`,
  cleared 0 @ `0x801E0AA4`, tested @ `0x801CB83C` and `0x801CB9B8`).
- `0x8015C69C[4]` (save `+0x94`) is the party-availability table.
- `func_801DD654_599564` has zero callers.

### INFERRED (single line of evidence, or reasoned)

- `action_id` 0 = normal idle, 1 = low-health idle, 0x42 = `player->0x59` special
  idle. The *idle* reading follows from being the swap's terminal states plus the
  HP-ratio selection between 0 and 1; the individual animation identities are not
  proven.
- Index→character mapping (0=Goemon, 1=Ebisumaru, 2=Sasuke, 3=Yae) — from
  `func_8000B640_C240` seeding slots 0 and 1 at new game.
- `*(u8*)(camtask + 0xD2)` = "camera busy / cutscene in progress".
- `task+0x38` with `+0x4C == 2` = a carried/mounted/attached object in a state
  that tolerates the owner changing.
- `*(u8*)0x800C7AE0` bit 0x4 = "C-button actions disabled by the current scene".
- Condition D relates to Ebisumaru's model/animation not being resident
  (`player->0x86 == 0xFFFF` = "no animation"); note `player->0x86` is explicitly
  zeroed by the commit function @ `0x801DD5FC`.

### Only settleable on device

- Whether the animation blends acceptably when a swap is started from a walk/run
  state (the state machine reinstalls an idle animation regardless).
- Whether the four characters' collision capsules differ enough to matter (see §7).
- The exact identity of `action_id = 0x42`.
- Whether the ~30-frame lock feels acceptable mid-combat.
- The precise index→character mapping.

---

## 7. Feasibility — what breaks if the gate is relaxed

### Safe to relax: other **grounded locomotion** states

Adding walk/run action ids to the whitelist is low-risk. The commit function
already zeroes velocity, so the character stops dead — cosmetically abrupt but
state-consistent. The state machine then reinstalls an idle animation and an
idle `action_id`, which is a *valid* terminal state regardless of what preceded
it. Nothing in the sequence reads the previous action.

**Cost: cosmetic only.** Player slides/snaps to a stop, plays the ~30-frame swap,
and stands idle. Not a corruption.

### Unsafe to relax: mid-air, mid-attack, and attached states

Concrete, specific problems, in decreasing order of severity:

1. **Attached-object states (ladders, ropes, vehicles, carried objects,
   Impact/mech, riding) — CAN CORRUPT.** Condition B (`task+0x38` → `+0x4C == 2`)
   exists precisely to reject these. That object holds a back-pointer
   relationship to the player; the swap destroys and rebuilds the player's model
   and animation without notifying it. **Do not remove Condition B.** It is not
   a motion gate and deleting it buys nothing.
2. **Mid-air — WILL LOOK BROKEN, may desync collision.** Velocity is zeroed
   (`0x68/0x6C/0x70/0xE8`) but nothing re-establishes a grounded flag or
   re-runs the ground query. Phase 5 does call `func_801CC454_...`, which may or
   may not resolve this. Expected outcome: the character freezes in mid-air for
   ~30 frames playing an idle stance, then either falls or snaps. Ugly; possibly
   recoverable. **Needs on-device testing before allowing.**
3. **Mid-attack / mid-damage — animation and hitbox desync.** The swap reinstalls
   an idle animation but does not tear down any active attack hitbox or
   invulnerability timer belonging to the outgoing character. Live hitbox state
   would persist against the new model. **Ugly to genuinely buggy.**
4. **Collision capsule differences.** MNSG's four characters have visibly
   different heights (Ebisumaru is short and wide, Yae is tall). If the capsule
   is derived from the loaded model rather than from a fixed constant, swapping
   inside geometry that only fits one of them can push the new character into a
   wall. The game's own "only from idle, only where you're standing" rule dodges
   this. **Not statically resolved** — I did not locate where the capsule
   dimensions are set. This is the biggest unknown and the most likely source of
   a real (not cosmetic) bug in a fully-unlocked swap.
5. **The ~30-frame input lock is unavoidable.** It is the ROM DMA + decompress +
   texture upload of a 96 KiB model. There is no "instant" swap available without
   a much larger change. This alone means "swap while moving" can never be
   *seamless* — the player will always stop and stand for half a second.
6. **Camera / movement basis:** no risk. The camera task is not re-pointed; the
   analog-camera work in `patches/camera.c` reads the live Camera struct via the
   camera task, which the swap does not touch.

### Verdict

**Relaxing the gate to "any grounded locomotion state" is worth doing and is
low-risk.** Fully removing `func_801DD3C4` is **not** recommended: keep
Conditions B, C and D, and keep the `player->0x69` re-entrancy lock. The swap
will still never be seamless because of the ROM load.

---

## 8. Recommended implementation

### Preferred: `RECOMP_PATCH` of `func_801DD3C4_5992D4`

This is the clean lever: one small leaf function, one caller, a pure predicate
with no side effects. Reimplement it faithfully in `patches/` and widen only
Condition A, keeping B/C/D verbatim.

Precedent for patching a **file_11 overlay** function already exists in this
repo: `patches/camera.c:322` is `RECOMP_PATCH s32 func_801CE3F0_58A300(u8* task, f32 speed)`,
and `0x58A300` is inside file_11 (`0x587370`–`0x5C8770`). The recompiled symbol
is present (`RecompiledFuncs/funcs.h:1388`), so the patch will link.

Sketch (do **not** treat as final code — the widened whitelist needs the real
locomotion action ids, which must be observed on device):

```c
/* patches/charswap.c */
#define G_CAMERA_TASK   (*(u8**)0x801FC624)

RECOMP_PATCH u32 func_801DD3C4_5992D4(u8 *task) {
    u8 act = *(u8*)(task + 0xCC);

    /* Condition A — widened. Vanilla: act < 2 || act == 0x42. */
    s32 act_ok = (act < 2) || (act == 0x42);
    if (!act_ok && recomp_get_freeswap_mode()) {
        act_ok = /* TODO: grounded locomotion ids, confirmed on device */ 0;
    }
    if (!act_ok) return 0;

    /* Condition B — KEEP VERBATIM (attached/mounted object). */
    u8 *o = *(u8**)(task + 0x38);
    if (o != NULL && *(u8*)(o + 0x4C) != 2) return 0;

    /* Condition C — KEEP VERBATIM (camera busy). */
    if (*(u8*)(G_CAMERA_TASK + 0xD2) != 0) return 0;

    /* Condition D — KEEP VERBATIM. */
    if (*(u8*)(task + 0x60) == 1) {
        u8 *player = *(u8**)(task + 0x5C);
        if (*(u16*)(player + 0x86) == 0xFFFF) return 0;
    }
    return 1;
}
```

Host-config plumbing, following the repo's established idiom (see
`goemon_cheats_re.md` §7 and the analog-camera wiring):
`recomp_get_freeswap_mode = 0x8F0000xx;` in `patches/syms.ld` → `extern "C"`
shim in `src/game/recomp_api.cpp` → `REGISTER_FUNC` in `src/main/main.cpp` →
`DECLARE_FUNC` in `patches/input.h` → a getter in `src/game/config.cpp` /
`include/goemon_config.h` → a toggle row in `assets/config_menu/cheats.rml`.

### Not recommended

- **Per-frame host-driven write** (the `patches/cheats.c` model). Writing
  `0x8015C5DC` directly does *not* swap the character — the model is only loaded
  by the state machine, so you would get the new id with the old model, a
  desynced HUD, and possibly a mismatched ability set. Do not use this approach.
- **Forcing the swap directly** by calling `func_801DD50C` from a per-frame hook.
  This would bypass `player->0x69` re-entrancy protection and could stack two
  swap state machines. If a host-triggered swap is ever wanted, it must still go
  through the gate + `func_801DD5C0` and must check `player->0x69 == 0` first.
- **Patching `func_801DCE10_598D20`** to change the trigger button. Possible, but
  it also carries the C-Up and C-Left actions; a mistake there breaks unrelated
  abilities. Only worth it if the user wants a *different* button.

### Input

**No new input is needed.** C-Down (held, `0x4`) is already the game's swap
button and is read by `func_801DCE10_598D20`. Two caveats worth surfacing:

- It is **level-triggered**, not edge-triggered — holding C-Down will re-fire the
  swap every frame the gate passes. Today the ~30-frame lock hides this. If the
  gate is widened it will still be hidden by the same lock, but if any future
  change shortens the sequence, C-Down should be converted to edge-triggered
  (`*(u16*)(rec + 0x4) & 0x4`).
- **Holding R suppresses it** (`held & 0x10` @ `0x801DCE4C`). Worth mentioning in
  any UI text.

---

## 9. Tooling

The `/tmp/g64/` toolkit from the cheats sweep was reused and extended
(regenerate if the tmpdir is cleared — `flat.txt` is the flat disassembly index,
`0xVRAM insn | func`):

- `an.py LO HI` — static access map by effective address (lui/addiu base tracking)
- `owner.py <needle>` — attribute referencing functions to overlay files
- `cal2.sh <addr> ...` — list callers of `jal 0x<addr>`, deduplicated by function
- `dd.sh <func_name> ...` — dump named functions from `flat.txt`

---

## 10. ON-DEVICE VERIFICATION (2026-07-18) — supersedes several inferences above

Captured with a temporary per-frame logger on `PlayerTask.action_id` (+0xCC),
correlated against processed-stick magnitude and against swap outcomes. This
section is empirical and **overrides the static guesses in §4 where they
conflict**.

### Verified action_id map

| id(s) | Meaning | Swap outcome |
|---|---|---|
| `0x00` | idle | fine (vanilla) |
| `0x0C`–`0x11` | movement by speed: creep (stick 1-18%), slow walk (19-27%), walk (29-50%), run (56-60%), decelerating, stopping | fine |
| `0x17`,`0x19`,`0x1A`,`0x1B`,`0x1C` | **rest of the same gait cycle** — logged cycling `0x17->0x19->0x1A->0x1C->0x0D->0x0F` while running | fine |
| `0x58`–`0x5E` | jump / attack family (`0x58`,`0x59`,`0x5A`,`0x5D`,`0x5E` each verified; `0x5B`/`0x5C` inferred from family) | fine |
| `0x40`, `0x43`, `0x71`, `0xA2` | unidentified action states, each swapped out of cleanly | fine |
| `0x35` ↔ `0x38` | **LADDER climb** (alternating while ascending/descending) | **BROKEN — see below** |
| `0xBA` | swap in progress | works, but re-entrant; refused on principle |

### The ladder failure (reproduced twice)

Swapping from `0x35`/`0x38` leaves the player **T-posed and floating**. First
occurrence: drifted upward continuously and required a force-close. Second:
floated until reaching level ground, then self-corrected — **but the ladder
object stayed unusable until the area was reloaded.** That persistent object
corruption, not the pose, is the reason this must stay blocked.

`attached_obj` was `no` on every ladder frame, so **condition B is NOT the
lever for ladders** (contradicting the §4 expectation that carried/mounted
guarding would cover them). An explicit `action_id` exclusion is required.

### Design consequence

The permitted set is a **whitelist**, deliberately. A blacklist would default
untested states (cutscenes, minigames, vehicles, boss sequences) to *allowed*,
and the ladder demonstrates that an untested state can corrupt world objects,
not merely look wrong. Unknown states must default to refused.

### Corrections to earlier sections

- §4's suggestion that `0x17`–`0x1C` are ladder/water states is **wrong** —
  they are locomotion. They were originally seen in a pass that included
  climbing and swimming, and were misattributed by timing.
- Swimming is **safe**; it needed no exclusion.
- The measured swap cost is **~0.93s** of locked input (30-frame lock plus a
  24-frame settle), not the ~30 frames implied by the input lock alone.
