# Goemon save path + safe-state gate (RE, 2026-07-18)

Done while porting Zelda64Recomp's autosave. Two questions: how does the game
commit a save, and how do we know when it is safe to do so.

Confidence is marked per claim. HIGH = read the recompiled C in
`RecompiledFuncs/`. MEDIUM = inferred. Anything MEDIUM below is worth one
on-device check.

## 0. Residency — correcting an earlier assumption

Only two sections are always resident (`Goemon64RecompSyms/mnsg.syms.toml`):

| section | vram | size |
|---|---|---|
| `.entry` | 0x80000400 | 0x50 |
| `.main`  | 0x80000450 | 0x7DBD0 (ends 0x8007E020) |

Everything else is a swappable module at a fixed vram:

- **Slot A @ 0x801CB460** — `.file_11` (the main 3D field/town engine, 0x41400),
  `.file_13`…`.file_16`, `.file_20`, `.file_22`, `.file_23`
- **Slot B @ 0x8020D2A0** — `.file_12` (menu/system), `.file_17`…`.file_19`, `.file_21`
- **Slot C @ 0x08000000** — `.file_24`…`.file_79`, the true relocatable overlays

**So base-exe *code* is `< 0x8007E020`.** Earlier notes calling `func_801CC4C0`,
`func_801CE3F0` or `func_80214D58` "base exe" are wrong — those are module code.
Base-exe *data* (the `System` struct at 0x8008CCC0, the 0x8015Cxxx globals) is
always resident. (HIGH)

## 1. Save path

Controller Pak, not SRAM. Pak identity from `func_80023410_24010`:
`game_code 0x4E473545 "NG5E"`, `company_code 0x4134 "A4"`. This promotes the
previously-MEDIUM note in `goemon_cheats_re.md` to **HIGH**.

Pak layout: a 0x100-byte header block at offset 0, then fixed 0x500-byte slots
at `0x100 + slot * 0x500`.

### Key globals (HIGH)

| Address | Meaning |
|---|---|
| `0x80168E84` | `PfsMgr *gPfsMgr` |
| `gPfsMgr + 0x3F0` | `s32 file_no` (set by FindFile) |
| `gPfsMgr + 0x3F4` | `void *xferBuf` |
| `gPfsMgr + 0x3FC` | `s32 lastError` — every wrapper's return |
| `0x8015C5C8` | pointer to the big global game struct (`G`) |
| `0x8015C608` | **gamedata**, 0x304 bytes (primary) |
| `0x8015C910` | shadow copy of gamedata |
| `0x8015CC18` | 12-byte header record written to pak block 0 |
| `0x801C7750` | overlay-side "last save result" mirror |
| `*(u32*)(G + 0x3B040)` | **selected save slot** — absolute `0x800C7D00`; see §4 |

### Always-resident wrappers (all in `.main`) (HIGH)

| Wrapper | osPfs call | Role |
|---|---|---|
| `func_800232EC_23EEC` | `osPfsIsPlug` | init mgr + plug check |
| `func_80023410_24010` | `osPfsFindFile` | **sets `gPfsMgr->file_no`** |
| `func_80023480_24080` | `osPfsFileState` | stat |
| `func_800234D8_240D8` | ReadWriteFile flag=0 | **READ one 0x500 slot** |
| `func_80023610_24210` | ReadWriteFile flag=1 | **WRITE one 0x500 slot** |
| `func_80023698_24298` | ReadWriteFile flag=1 | write 12-byte header block |
| `func_80023A1C_2461C` | — | **CRC-32/MPEG-2** |
| `func_8000B718_C318`  | — | live -> gamedata marshal |
| `func_800148F0_154F0` / `func_80014B74_15774` | — | arena alloc / free |

`s32 func_80023610_24210(void *buf, u32 slot)` — `buf` is exactly 0x500 bytes.
`u32 func_80023A1C_2461C(const void *data, u32 len)` — init 0xFFFFFFFF,
poly 0x04C11DB7, MSB-first, no reflection, **no final XOR** on the value stored
(the routine returns `~crc`).

Beware: `func_80023B60_24760`, `func_80023C14_24814`, `func_80023CC8_248C8` are
base-exe *pak diagnostics* that write an incrementing byte pattern to slot 0 —
not saves. (HIGH)

### The real save routine — `func_80214D58_5D0228` (HIGH)

`RecompiledFuncs/funcs_53.c:3776-4111`. Lives in **`.file_12` (slot B)**.

**Corrected 2026-07-18:** it has zero `jal` callers, but it is *not* "dispatched
from the save-menu state machine". It is invoked **only by the event-script (GEV)
VM's `0x800B` "call native function" opcode**, from **12 script call sites** —
see §5. The script VM itself (`func_8003D68C_3E28C` and friends) is in `.main`.
(HIGH)

Its sequence:

```
1. func_800232EC_23EEC() / func_80023410_24010() / func_80023480_24080()
     any non-zero -> mirror lastError to 0x801C7750 and return it
2. buf = func_800148F0_154F0(G + 0xC7FA4, 0xA00);  if (!buf) return -1
   verify = buf + 0x500
3. func_8000B718_C318()                      // live -> gamedata
4. memcpy(buf + 4, 0x8015C608, 0x304)
5. *(u32*)buf = func_80023A1C_2461C(buf + 4, 0x304)
6. func_80023610_24210(buf, slot)            // WRITE
7. func_800234D8_240D8(verify, slot)         // READ BACK
8. compare 0xC2 words (0x308); mismatch -> lastError = 6
9. func_80023698_24298(0x8015CC18)           // header: see caveat below
   lastError = 0; func_8002338C_23F8C()      // (a stub: jr ra; nop)
10. func_80014B74_15774(G + 0xC7FA4, buf)    // free
11. *(u32*)0x801C7750 = lastError; return it
```

Block layout: `[crc32 over the next 0x304 bytes][0x304 gamedata][padding to 0x500]`.
The CRC covers **only the 0x304 data at buf+4**, not the whole 0x500.

### The header write leaks stack — `func_80023698_24298` (HIGH)

Calling it "write the 12-byte header block" understates what it does, in a way
that breaks naive file comparison. `RecompiledFuncs/funcs_16.c:9535`:

- copies 12 bytes from its argument (`0x8015CC18`) to `sp+0x50`
- CRC-32s those 12 bytes into `sp+0x4C`
- writes **`0x100` bytes at raw file offset 0**, sourced from `sp+0x4C`

Only the first `0x10` bytes of that window are ever initialised:

| file offset | content | real? |
|---|---|---|
| `0x00`-`0x03` | CRC-32 of the 12 bytes | yes |
| `0x04`-`0x0F` | the three words from `0x8015CC18` | yes |
| **`0x10`-`0xFF`** | **uninitialised stack residue** | **no** |

The residue is leftover RDRAM pointers and locals — measurable as changing bytes
whose *high* byte stays constant (`0x80xxxxxx`). Nothing reads past `0x0F`: the
header's own reader `func_80023560_24160` CRC-checks only `0x04`-`0x0F` and
copies out 12 bytes. Both boot callers respond to a CRC mismatch identically —
`memset(0x8015CC18, 0, 12)` and continue. **No load path rejects a file on
header contents.** (HIGH)

The 12 real bytes are three *global* progress/unlock flags, **not** per-file
summary data. Sole reader is the file-select overlay's `func_801D21B8_665068`,
which ORs them with live globals to grey out menu entries.

**Consequence for testing:** an autosave and a menu save of *identical* game
state will still differ in up to `0xF0` header bytes, because the residue
depends on what last ran at that stack depth. A whole-file `cmp` therefore
reports a **false failure**. Compare the slot region only — `cmp -i 256`.
Confirmed on device: bytes `0x00`-`0x0F` were byte-identical across both
writers, and the first differing byte was exactly `0x10`.

Evidence is committed under `docs/re-notes/fixtures/` — `06-autosave-C.bin` vs
`07-npcsave-cross.bin` for the cross-writer case, and
`08-npcsave-consecutive-A/B.bin` for the game path's own 15-byte header churn.
See that directory's README for the reproduction commands.

`func_8000B718_C318` (base exe, `funcs_3.c:6588`) marshals live state into
gamedata: indexes a 10-byte-stride table at `0x8005BA30` by the stage id at
`gamedata+0x200`; `memcpy(0x8015C5D8 -> gamedata+0x64, 0x30)`; clamps
`gamedata+0x78` to >= 3 and `+0x70` to >= 10; finally mirrors gamedata into the
shadow at `0x8015C910`. Its copy helper `func_80004208_4E08` is **bcopy order
`(src, dst, n)`** — getting that backwards inverts primary/shadow.

**Correction to `goemon_cheats_re.md`:** that note says `func_8000B718_C318`
commits the live block "on area transitions". It does not — it is a save-time
commit. The `G_MONEY_SAVE` sync in `patches/cheats.c` is still correct, just for
a different reason. (HIGH)

**Correction to the above correction (2026-07-18):** "the save routine is its
*only* caller in the whole binary" is true for MIPS `jal` (1 hit) but false
overall — `0x8000B718` appears **24 times** as a GEV script native-call operand:
two per save script. One is the full save path; the other is a **marshal-only,
no-pak-write** block. That second one is the in-game *"The present situation will
be Saved… However, turning the power off or resetting will Erase the Saved
information"* option — a **RAM-only suspend**, which is what that message means.
The conclusion (not an area-transition commit) is unaffected. (HIGH)

### Why reimplementing is safe here (HIGH)

On real hardware `osPfsReadWriteFile` blocks on the SI event queue and takes
multiple frames. In this port `librecomp/src/pak.cpp` stubs the entire API
synchronously — FindFile always returns PFS_OK with `file_no = 0`, IsPlug always
reports plugged, and ReadWriteFile is a direct `save_read`/`save_write`. So the
sequence is instantaneous and safe to run from a per-frame function.

**Hard requirement:** the shim does `assert(file_no == 0)`, so the probe chain
(steps 1) must run before any read/write.

## 2. Safe-state gate

Goemon's equivalent of MM's `KaleidoSetup_Update` is
**`func_8001F940_20540`** (0x8001F940, `.main`, always resident,
`RecompiledFuncs/funcs_0.c:3410`). It is entry 0 of the base-exe game-state jump
table at `0x8006B730`, dispatched by `func_8001F8B0_204B0`. (HIGH)

Unlike MM, its START test sits *second* in the chain, so the fall-through is not
directly latchable. Every guard it reads is a fixed base-exe address, so
`patches/autosave.c` re-evaluates the predicate instead of patching it.

| Address | Meaning |
|---|---|
| `0x800C7A94` | **top-level game state index; gameplay == `0x0D`** (confirmed on device) |
| `0x800C7A9E` | gameplay **phase**; running == **1** (see correction below) |
| `0x800C7A9F` | inner state machine has requested exit |
| `0x800C7AE6` | pause menu open (only 4 refs ROM-wide — unusually clean) |
| `0x800C7AD6` | file load / area transition in progress (MEDIUM) |
| `0x800C7AE9` | event/script lock (MEDIUM) |
| `0x800C7AA4` | sub-mode; gate wants `< 4 && != 3` (MEDIUM) |
| `0x80077858` | what `func_8003F1D8_3FDD8` tests |
| `0x8015C5D4` | `g_is_loading_file` |

These are `System` fields; `System` is the base-exe static at `0x8008CCC0`
(`0x800C7A9E` = `System + 0x3ADDE`).

### Correction (2026-07-18): `0x800C7A9E` is a phase counter, and 0 is NOT gameplay

This note previously said `0x800C7A9E` is a "game-state stack index; 0 == normal
gameplay". **The polarity was inverted, and it made the autosave refuse every
save** — the guard demanded the one frame the value is 0.

There are two levels, both byte indices into jump tables:

**Top level** — `0x800C7A94`, dispatched by `func_80002040_2C40` through an
18-entry table at `0x80058908`. Gameplay is entry **`0x0D`**; the preceding state
`func_80002E70_3A70` ends with `func_80003728_4328(0xD)`. Entry `0x11` also
dispatches the same inner machine as a bare variant with no exit handling.
**Confirmed on device: reads 13 during ordinary gameplay.** (HIGH)

**Inner phase** — `0x800C7A9E`, read as a byte by `func_8001F8B0_204B0` to index
a **3-entry** table at `0x8006B730`:

| value | entry | function | meaning |
|---|---|---|---|
| 0 | 0 | `func_8001F914_20514` | init — runs **one frame**, then bumps to 1 |
| **1** | 1 | **`func_8001F940_20540`** | **running gameplay** — the gate below |
| 2 | 2 | `func_8001FA3C_2063C` | teardown; sets `0x800C7A9F = 1` |

Entry 0 calling `func_80003478_4078` (which increments `0x800C7A9E`) is the
decisive proof. Exhaustive whole-ROM scan finds exactly three writers — a
set/inc/dec accessor trio at `0x80003428` / `0x80003478` / `0x800034D0`; the dec
has **zero callers** and is dead code. (HIGH)

`0x800C7A9F` is likewise **not** "mode changed this frame": it is cleared by
every level-setter cascade and set to 1 only by the teardown entry. Its sole
reader, `func_80002EFC_3AFC`, switches top-level state when it is set.

**One more trap in the same chain:** the gate's `andi 0x1000` at `0x8001F96C`
reads `0x800C7D3C`, a **controller button word**, and `0x1000` is `CONT_START`.
That is the game's *"did the player ask to pause"* test — a **trigger**, not a
safety condition. Adding it as a guard refuses every save (observed: the live
word read `0x0010` = `CONT_R` from a held combo). Do not reintroduce it.

Stage type == which module occupies slot A, so `D_80054ACC_556CC[11].start != 0`
restricts to the main 3D field engine, excluding the Impact battle, sidescroller
and minigame engines. (MEDIUM)

### `*0x8020CA2C` is NOT a safe-gameplay gate (HIGH)

`patches/cheats.c` and `patches/camera.c` gate on this word being a plausible
RDRAM pointer. Reading its only writer (`func_801CC4C0_5883D0` at `0x801CC79C`),
it stores `&g_system->controllers[player->0x90]` — and **nothing ever clears
it**. So it stays "valid" through pause menus, dialogue and cutscenes, and reads
as stale-but-plausible after a module swap. It is really a "`.file_11` has run at
least once" flag.

Adequate for cheats/camera (worst case you write to a struct that exists
anyway); **not** adequate for gating a save. `patches/autosave.c` deliberately
does not use it.

**Correction to `goemon_global_camstate.md` §5:** the "static table of 0x18-byte
camera-direction records at 0x800C7DB0" is the `System` controller array
(`sizeof(Controller) == 0x18`), so `+0xC/+0x10/+0x14` are `stick_x`, `stick_y`,
`stick_magnitude`.

## 3. Open / lowest confidence

- The semantics of `0x800C7AA4` (`< 4 && != 3`) and whether `0x800C7AD6` really
  is the loader counter. Partially settled on device: pressing the save combo
  mid-dialogue with a save NPC refused with `sub 3` **and** a non-zero
  `0x80077858` (the script-VM gate), so the chain does exclude dialogue.
- Whether top-level state `0x11` (the bare gameplay variant) is ever live during
  saveable play. `patches/autosave.c` accepts only `0x0D`; if a refusal ever
  logs `state 17`, that is the line to revisit.

## 4. The slot cursor — `0x800C7D00` (2026-07-18)

Absolute address derived and verified: `G = *(0x8015C5C8) = 0x8008CCC0`, cursor
at `G + 0x3B040` = **`0x800C7D00`**. It is a field *inside* the `System` struct,
so it is covered by the boot memset (`func_80004170_4D70` zero-fills `0xCF908`
bytes from `0x8008CCC0`). **It is 0 at boot, guaranteed.** (HIGH)

**Exactly three writers exist**, all in the Adventure Diary overlay (`file_id
0xF`, vram base `0x801CB460`), all dispatched as per-frame object handlers:

| fn | store | value |
|---|---|---|
| `func_801CEC38_661AE8` | `0x801CED80` | menu index − 2 (the file-select proper) |
| `func_801CDC70_660B20` | `0x801CDCC0` | **−1** ("Controller Pak not connected, OK?") |
| `func_801CE1F0_6610A0` | `0x801CE37C` | **−1** ("Not use Controller Pak") |

**Sole reader in the entire ROM:** `func_80214D58_5D0228`.

Three consequences for `patches/autosave.c`:

1. **The new-game path DOES set the cursor.** The empty-slot branch and the load
   branch converge and both fall through to the unconditional store at
   `0x801CED80`. That suspected hole is closed. (HIGH)
2. **`-1` is a real sentinel**, so the bounds check is load-bearing — and it must
   stay **unsigned**. `slot >= SAVE_SLOT_COUNT` on a `u32` rejects `0xFFFFFFFF`;
   a signed compare would let it through and compute pak offset
   `-1 * 0x500 + 0x100`. The game's own routine has **no bounds check at all**;
   it relies on never offering the save UI while the cursor is `-1`.
3. **The real hazard is not "stale or garbage" — it is `0`.** The Diary UI is
   reachable only *from* the main-engine scene, so a fresh boot reaches that
   scene with the cursor never written. `0` is a **valid** index, so the
   `>= 3` guard structurally cannot catch it: such a save silently targets
   **file 1**. (MEDIUM-HIGH — the structural reachability is proven; whether
   that window is *saveable* gameplay was not separately established, and the
   `autosave_is_safe()` chain adds further conditions.)

Mitigation if that window is ever shown to be reachable: all three writers
converge on `func_801CD890_660740`, so a single hook there could latch "a
selection was committed this boot" and refuse until set.

**`SAVE_SLOT_COUNT = 3` is CONFIRMED** (2026-07-18) — the Select Adventure Diary
screen shows exactly three entries, the third reading "From the beginning".

## 5. When the game saves (2026-07-18)

**It never saves on its own.** Every Controller Pak write in the ROM is
player-initiated. (HIGH)

`func_80214D58_5D0228` is reached only via the GEV script VM's `0x800B` opcode,
from **12 call sites**: 6 save NPCs/signposts, 5 inns, and one second prompt
sharing a script file. All 12 sit behind a *"Do you wish to save…? Yes/No"* or an
inn's A-confirm. String counts corroborate exactly: `"Do you wish to save"` ×6,
`"Spend the night"` ×5.

The only other pak writers are the `.file_15` Diary management menu (new / copy /
erase) and the base-exe pak *diagnostics* — both already documented above.

### Flush sources — the closed set (HIGH)

Every write of a `0x500` slot goes through `func_80023610_24210`, and it has
**exactly 8 static call sites** across 5 recompiled files. Verified by grep over
`RecompiledFuncs/` (a 9th mention in `funcs_6.c` is the definition itself):

| sites | file | where | save-class? |
|---|---|---|---|
| 1 | `funcs_53.c` | `func_80214D58_5D0228`, the real save | **yes** |
| 5 | `funcs_84.c` ×1, `funcs_85.c` ×4 | `.file_15` Diary management — new file, copy, erase | **yes**, deliberate |
| 2 | `funcs_3.c`, `funcs_4.c` | base-exe pak **diagnostics** (`func_80023C14_24814`, `func_80023CC8_248C8`) — write an incrementing byte pattern to slot 0 | **NO** |

This set being closed is what makes the `.manual.bak` design in `docs/autosave.md`
sound: with all flush sources known there is no unmapped writer left to consume an
armed one-shot. The two diagnostic writers are the exception that needs handling
— **whether they are reachable at runtime is not established** and is a listed
precondition on that design.

**Consequence for autosave design:** there are **no natural commit points to
hook**. A timer is the correct trigger; this is settled, not provisional.

The per-file summary on the Diary screen comes from the **payload**, not the
header:

| field | payload offset |
|---|---|
| hearts (max HP) | `+0x6C` — also the empty-file test (`== 0`) |
| money (ryo) | `+0x74` |
| lives | `+0x78` |
| location / stage id | `+0x200` |
| play time (frames) | `+0x264` |

Two more mapped empirically on 2026-07-18 by diffing two autosaves either side of
a real play session (`fixtures/06-autosave-C.bin` vs `10-autosave-statechange.bin`),
neither of which is read by the file-select display:

| field | payload offset | evidence |
|---|---|---|
| **current HP** | `+0x70` | `11 -> 10` across a session in which the player confirmed taking damage; `+0x6C` (max HP) stayed `14`. (HIGH) |
| unidentified counter | `+0x7C` | `0 -> 2` over the same session. Possibly deaths or a rest/checkpoint count — **not identified**, do not rely on it. (MEDIUM: that it changed is observed; what it means is not) |

Note `+0x6C` being *max* HP is why taking damage does not move it — a trap when
choosing a field to exercise in a differential test.

That whole play session changed only **10 slot bytes**: those four fields plus
the CRC word. The save is otherwise byte-stable across normal play, which is what
makes the differential test sharp.

Slot validity is purely per-slot: empty iff `payload[0x6C] == 0`, corrupt iff
`crc32(payload, 0x304) != *(u32*)(payload-4)`. Neither consults the header.

**Incidental original-game bug (HIGH):** the copy/erase refresh path
`func_801CE87C_66172C` reads the location id from `payload+0x294`, where the
other two display sites read `+0x200` — so a freshly copied file shows a wrong
location name. Unrelated to autosave; logged for whoever hits it.
