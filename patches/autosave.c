#include "patches.h"
#include "autosave.h"
#include "misc_funcs.h"

// Autosave. Ported from Zelda64Recomp's patches/autosaving.c, which does NOT
// call the game's own save routine but reimplements it inline; the same applies
// here, and for the same reason: Goemon's save routine func_80214D58_5D0228
// lives in an overlay rather than in always-resident code.
//
// Corrected 2026-07-18: that overlay is .file_12 (slot B), NOT file_23 (which is
// a slot-A module), and the routine is not "dispatched indirectly" from a menu
// state machine -- it has zero jal callers and is invoked ONLY by the event
// script (GEV) VM's 0x800B native-call opcode, from 12 script sites (6 save
// NPCs/signposts, 5 inns, +1). See docs/re-notes/goemon_save_re.md sections 1
// and 5.
//
// Caveat on the rationale: .file_12 may in fact be resident during field
// gameplay (slot-A field modules carry ~2166 references into slot B, and the
// save scripts run while the player is walking around), which would make a
// direct call viable. MEDIUM confidence -- the module loader was not traced.
// The reimplementation is still the safer choice; only the stated reason for it
// is less solid than it reads.
//
// It does not need to be. Every function that routine calls is base exe and
// always resident, so the sequence is reproduced below from its recompiled C
// (RecompiledFuncs/funcs_53.c:3776-4111). See docs/re-notes/goemon_save_re.md.
//
// Because this drives the game's own buffer through the game's own wrappers,
// a resulting save is indistinguishable from one made at a save point: the
// normal load path accepts it and starts the player wherever a normal save
// would. No custom slot, format, checksum or respawn handling is involved.

// ---------------------------------------------------------------------------
// Base-exe save wrappers. Plain C declarations resolved by the recompiler from
// the symbol file (the established idiom -- cf. patches/camera.c's declaration
// of func_801CE4D0_58A3E0). Argument shapes are recovered from the register
// usage at each call site in the original routine.
// ---------------------------------------------------------------------------

// Pak availability / probe chain. Each returns 0 when it is OK to proceed.
s32 func_800232EC_23EEC(void);
s32 func_80023410_24010(void);
s32 func_80023480_24080(void);

// Arena allocator pair used for the transfer buffer.
void* func_800148F0_154F0(void* arena, u32 size);
void  func_80014B74_15774(void* arena, void* ptr);

// Copies the live player block into the save buffer (live + 0x94). The save
// routine is its only MIPS jal caller, but not its only caller: 0x8000B718
// appears 24 times as a GEV script native-call operand -- two per save script.
// The second one is a marshal-only block with no pak write: the in-game
// "present situation will be Saved / power off will Erase it" RAM-only suspend.
void func_8000B718_C318(void);

// CRC-32, MSB-first, poly 0x04C11DB7, init ~0, final complement. Verified
// against its recompiled body at RecompiledFuncs/funcs_12.c:6367.
u32 func_80023A1C_2461C(const void* data, u32 len);

// Pak file write / read-back of one 0x500-byte slot. The pak byte offset is
// slot * 0x500 + 0x100. Both return the pak manager's last error, 0 on success.
s32 func_80023610_24210(void* buf, u32 slot);
s32 func_800234D8_240D8(void* buf, u32 slot);

// Post-write commit, then release. Return 0 on success.
s32 func_80023698_24298(void* p);
void func_8002338C_23F8C(void);

// ---------------------------------------------------------------------------
// Fixed addresses, all base-exe statics.
// ---------------------------------------------------------------------------

// Context pointer the original routine derives both the arena and the selected
// save slot from (lui 0x8016 / lw -0x3A38). The slot cursor is owned by the
// file-select UI, so this follows whichever slot the player loaded.
#define G_SAVE_CTX_PTR   (*(volatile u32*)0x8015C5C8)
#define G_ARENA_OFFSET   0xC7FA4
#define G_SLOT_OFFSET    0x3B040

// The in-RAM save buffer ("gamedata"), 0x304 bytes. Documented in
// docs/re-notes/goemon_cheats_re.md.
#define G_SAVE_BUFFER    ((const void*)0x8015C608)
#define G_SAVE_SIZE      0x304

// Status block; the save result is written to +0x3FC and mirrored to 0x801C7750.
#define G_STATUS_BLOCK   (*(volatile u32*)0x80168E84)
#define G_STATUS_OFFSET  0x3FC
#define G_STATUS_MIRROR  (*(volatile u32*)0x801C7750)

// Argument to the post-write commit call (lui 0x8016 / addiu -0x33E8).
#define G_COMMIT_ARG     ((void*)0x8015CC18)

// Transfer buffer layout: 0xA00 allocated, the image built at +0, and the
// read-back verification copy at +0x500. The image is one CRC word followed by
// the save buffer, and the original compares 0xC2 words (0x308 bytes) of it.
#define XFER_ALLOC_SIZE  0xA00
#define XFER_VERIFY_OFF  0x500
#define XFER_VERIFY_WORDS 0xC2

// Number of save slots. Derived from the pak file size the runtime's shim
// reports in osPfsFileState (0x1000, librecomp/src/pak.cpp): the layout is a
// 0x100-byte header block followed by 0x500-byte slots, and
// 0x100 + 3 * 0x500 == 0x1000 exactly. MEDIUM confidence -- derived from the
// file size rather than read out of the game's own slot-select code.
#define SAVE_SLOT_COUNT  3

// Returned when the slot cursor is not a usable slot index. Distinct from the
// pak manager's own error codes so it is identifiable in the log.
#define AUTOSAVE_ERR_BAD_SLOT (-2)

// ---------------------------------------------------------------------------
// Safe-state gate.
//
// This is the port of Zelda64Recomp's gCanPause trick: rather than enumerating
// unsafe states, reuse the predicate the game itself uses to decide whether the
// player may open the pause menu right now. In Goemon that decision lives in
// func_8001F940_20540 (0x8001F940, inside .main, always resident) -- the
// equivalent of MM's KaleidoSetup_Update.
//
// Its START-button test sits second in the chain rather than last, so the
// fall-through branch is not directly latchable the way MM's is. Instead the
// predicate is re-evaluated here: every guard it reads is a fixed base-exe
// address, so this is a pure data read needing no patch of the function.
//
// Deliberately NOT used here: *0x8020CA2C, the gate patches/cheats.c and
// patches/camera.c use. That word is a pointer to &g_system->controllers[i] and
// is never cleared once written, so it stays "valid" through pause menus,
// dialogue and cutscenes. It is adequate for those features but would not
// exclude anything that matters for a save.
// ---------------------------------------------------------------------------

// System struct (base-exe static at 0x8008CCC0) fields, by absolute address.
//
// G_STATE is the TOP-LEVEL game state index, dispatched by func_80002040_2C40
// through an 18-entry table at 0x80058908. Gameplay is entry 13 (0x0D); the
// state before it, func_80002E70_3A70, ends with func_80003728_4328(0xD).
// NOTE: entry 17 (0x11) also dispatches the same gameplay state machine as a
// bare variant with no exit handling. Only 0x0D is accepted here, since a
// wrongly-refused save is logged and recoverable while a save committed from an
// unexamined state is not. If the refusal printf ever reports state 17, this is
// the line to revisit.
#define G_STATE      (*(volatile u8 *)0x800C7A94)  // top-level state; gameplay == 0x0D
//
// G_PHASE is NOT a state stack index -- it is the 3-phase init/run/exit counter
// belonging to the gameplay state, read as a byte by the inner dispatcher
// func_8001F8B0_204B0 to index a 3-entry table at 0x8006B730:
//   0 = init frame  (entry 0, func_8001F914_20514, runs once then bumps to 1)
//   1 = RUNNING GAMEPLAY  (entry 1, func_8001F940_20540 -- the gate below)
//   2 = teardown    (entry 2, func_8001FA3C_2063C, sets G_PHASE_DONE = 1)
// The earlier note claiming "0 == normal gameplay" was inverted: 0 is the
// single init frame. Requiring 0 refused every save. Confirmed on device --
// gameplay reads 1.
#define G_PHASE      (*(volatile u8 *)0x800C7A9E)  // gameplay phase; running == 1
//
// Not "mode changed this frame": cleared by every level-setter cascade and set
// to 1 only by the teardown entry. It means "the inner state machine has
// finished"; its sole reader (func_80002EFC_3AFC) switches top-level state on it.
#define G_PHASE_DONE (*(volatile u8 *)0x800C7A9F)  // inner machine requested exit
#define G_PAUSED     (*(volatile u16*)0x800C7AE6)  // pause menu open (read as lhu: covers 0xAE6+0xAE7)
#define G_XFER       (*(volatile u16*)0x800C7AD6)  // file load / area transition in progress
#define G_LOCK       (*(volatile u8 *)0x800C7AE9)  // event/script lock
#define G_SUBMODE    (*(volatile s8 *)0x800C7AA4)  // sub-mode; the gate wants < 4 and != 3

// Non-System guards read by the same gate.
#define G_BUSY       (*(volatile u32*)0x80077858)  // what func_8003F1D8_3FDD8 tests
#define G_LOADING    (*(volatile u8 *)0x8015C5D4)  // g_is_loading_file
#define G_GUARD_A    (*(volatile u8 *)0x8015C562)
#define G_GUARD_B    (*(volatile u8 *)0x8015C54D)

// Controller button word, read by the gate at 0x8001F96C as a halfword. The
// gate's `andi 0x1000` against it is CONT_START -- this is the game's
// START-button test, i.e. "did the player ask to open the pause menu".
//
// It is deliberately NOT a guard here. It is a TRIGGER condition, not a safety
// condition: requiring it would mean only ever autosaving on the exact frame
// START is pressed, which is unsatisfiable while holding the save combo. It was
// briefly added as a guard on a misreading and refused every save; the live
// value read 0x0010 (CONT_R, from the combo) against a demanded 0x1000.
// Kept only as a diagnostic -- it usefully confirms the combo is reaching the
// game's own button word.
#define G_BUTTONS    (*(volatile u16*)0x800C7D3C)

static s32 autosave_is_safe(void) {
    // Top-level state must be gameplay. This check was absent entirely from the
    // first implementation. The constant was inferred statically (the state
    // before gameplay ends with func_80003728_4328(0xD)) and is now CONFIRMED on
    // device: the refusal printf reported state 13 during ordinary gameplay.
    if (G_STATE != 0x0D) {
        return false;
    }

    // Running gameplay: not the init frame (0), not teardown (2), and the inner
    // state machine has not requested exit.
    if (G_PHASE != 1 || G_PHASE_DONE != 0) {
        return false;
    }

    // The game's own pause-gate conjunction.
    if (G_PAUSED != 0 || G_LOCK != 0 || G_XFER != 0) {
        return false;
    }
    if (G_SUBMODE == 3 || G_SUBMODE >= 4) {
        return false;
    }
    if (G_BUSY != 0 || G_LOADING != 0) {
        return false;
    }
    if (G_GUARD_A != 0 || G_GUARD_B != 0) {
        return false;
    }

    // Restrict to the main 3D field/town engine (.file_11). The stage type in
    // this game is "which module occupies the slot at 0x801CB460", so the Impact
    // battle, sidescroller and minigame engines are excluded by requiring
    // file_11 to be the loaded one.
    if (D_80054ACC_556CC[11].start == 0) {
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// The save itself.
// ---------------------------------------------------------------------------

s32 goemon_save_now(void) {
    volatile u32* status;
    void* arena;
    u32* image;
    u32* verify;
    u32 slot;
    s32 result;
    s32 i;

    // Pak probe chain. Beyond mirroring the original, this is a hard
    // requirement: func_80023410_24010 is what sets the pak manager's file_no,
    // and the runtime's pak shim asserts file_no == 0 on every read/write.
    // The original bails to the shared exit on any non-zero result, reporting
    // whatever status the wrappers left behind.
    if (func_800232EC_23EEC() != 0 ||
        func_80023410_24010() != 0 ||
        func_80023480_24080() != 0) {
        // The probes populate the manager pointer, but if the very first one
        // bailed it may still be null -- and 0 + 0x3FC is a legal RDRAM offset,
        // so an unguarded read here would silently return garbage rather than
        // fault.
        if (G_STATUS_BLOCK == 0) {
            return -1;
        }
        status = (volatile u32*)(G_STATUS_BLOCK + G_STATUS_OFFSET);
        G_STATUS_MIRROR = *status;
        return (s32)*status;
    }

    if (G_STATUS_BLOCK == 0 || G_SAVE_CTX_PTR == 0) {
        return -1;
    }

    // Validate the slot cursor BEFORE allocating or marshalling, so a bad slot
    // cannot mutate gamedata or its shadow copy either.
    //
    // This guard is load-bearing, not defensive tidiness. The cursor is written
    // only by the save-menu UI, so on a file that has never opened that menu it
    // may be stale or uninitialised. Nothing downstream re-checks it: the pak
    // offset is slot * 0x500 + 0x100, and the single bounds check in the whole
    // chain is an assert() in librecomp's save_write, which is compiled out --
    // the Android build sets CMAKE_BUILD_TYPE=RelWithDebInfo, so -DNDEBUG is in
    // the flags. The write past it is an unchecked std::vector operator[].
    // Unguarded, an in-range wrong slot silently overwrites a different save,
    // and an out-of-range one is an out-of-bounds heap write.
    slot = *(volatile u32*)(G_SAVE_CTX_PTR + G_SLOT_OFFSET);
    if (slot >= SAVE_SLOT_COUNT) {
        return AUTOSAVE_ERR_BAD_SLOT;
    }

    arena = (void*)(G_SAVE_CTX_PTR + G_ARENA_OFFSET);
    image = (u32*)func_800148F0_154F0(arena, XFER_ALLOC_SIZE);
    if (image == NULL) {
        return -1;
    }
    verify = (u32*)((u8*)image + XFER_VERIFY_OFF);

    // Marshal live state into gamedata before snapshotting it. This resolves
    // the stage id at gamedata+0x200 against the table at 0x8005BA30, copies
    // the 0x30-byte live block to gamedata+0x64, applies the lives/health
    // floors, and mirrors gamedata into its shadow at 0x8015C910. Skipping it
    // would persist a stale block.
    //
    // Note this is a SAVE-time commit, not an area-transition one. (The comment
    // in patches/cheats.c saying otherwise is inaccurate -- see
    // docs/re-notes/goemon_save_re.md.) The game's save routine is its only jal
    // caller; its other 24 call sites are all GEV script native-calls at the
    // same 12 save NPCs, so it is still never an area-transition commit.
    func_8000B718_C318();

    // Image = CRC word followed by the save buffer.
    memcpy((u8*)image + 4, G_SAVE_BUFFER, G_SAVE_SIZE);
    image[0] = func_80023A1C_2461C((const u8*)image + 4, G_SAVE_SIZE);

    result = func_80023610_24210(image, slot);
    if (result == 0) {
        result = func_800234D8_240D8(verify, slot);
    }

    if (result == 0) {
        // Read-back verification. The original reports status 6 on mismatch.
        for (i = 0; i < XFER_VERIFY_WORDS; i++) {
            if (image[i] != verify[i]) {
                *(volatile u32*)(G_STATUS_BLOCK + G_STATUS_OFFSET) = 6;
                result = 6;
                break;
            }
        }
    }

    if (result == 0) {
        if (func_80023698_24298(G_COMMIT_ARG) == 0) {
            *(volatile u32*)(G_STATUS_BLOCK + G_STATUS_OFFSET) = 0;
            func_8002338C_23F8C();
        }
    }

    func_80014B74_15774(arena, image);

    status = (volatile u32*)(G_STATUS_BLOCK + G_STATUS_OFFSET);
    G_STATUS_MIRROR = *status;
    return (s32)*status;
}

// ---------------------------------------------------------------------------
// Per-frame poll.
// ---------------------------------------------------------------------------

// Manual test trigger: L + R + D-Up. Step 1 of the rollout is manual-only, so
// the timed autosave is deliberately not wired up yet -- the write path is
// proven on device against a backed-up save first.
#define AUTOSAVE_TEST_COMBO (L_TRIG | R_TRIG | U_JPAD)

void update_autosave(void) {
    static s32 combo_was_held = 0;
    u16 held;
    s32 combo_held;

    if (!recomp_get_autosave_enabled()) {
        combo_was_held = 0;
        return;
    }

    held = D_8008CCC0_8D8C0.controller[0].button_held_down;
    combo_held = ((held & AUTOSAVE_TEST_COMBO) == AUTOSAVE_TEST_COMBO);

    // Edge-triggered, so holding the combo saves once rather than every frame.
    if (combo_held && !combo_was_held) {
        if (autosave_is_safe()) {
            s32 status = goemon_save_now();
            if (status == AUTOSAVE_ERR_BAD_SLOT) {
                recomp_printf("[autosave] refused: no valid save slot selected "
                              "(cursor %d, need < %d)\n",
                              (s32)*(volatile u32*)(G_SAVE_CTX_PTR + G_SLOT_OFFSET),
                              SAVE_SLOT_COUNT);
            } else {
                recomp_printf("[autosave] manual save -> status %d (slot %d)\n",
                              status,
                              (s32)*(volatile u32*)(G_SAVE_CTX_PTR + G_SLOT_OFFSET));
            }
        } else {
            // Every guard is printed, not just the ones failing at the time of
            // writing. The failure mode of a wrong constant here is that every
            // save is silently refused -- which looks exactly like a gate
            // working correctly. This has already paid for itself twice: it
            // confirmed G_STATE == 13, and it caught a bogus guard on the
            // START-button bit in a single test cycle (btn read 0x0010 against
            // a demanded 0x1000). Keep every guard represented here.
            recomp_printf("[autosave] refused: unsafe state "
                          "(state %d want 13 | phase %d want 1 | done %d | "
                          "paused %d xfer %d lock %d sub %d | "
                          "btn %04X | "
                          "busy %d loading %d gA %d gB %d | file11 %d)\n",
                          (s32)G_STATE, (s32)G_PHASE, (s32)G_PHASE_DONE,
                          (s32)G_PAUSED, (s32)G_XFER, (s32)G_LOCK, (s32)G_SUBMODE,
                          (u32)G_BUTTONS,
                          (s32)G_BUSY, (s32)G_LOADING,
                          (s32)G_GUARD_A, (s32)G_GUARD_B,
                          (s32)(D_80054ACC_556CC[11].start != 0));
        }
    }

    combo_was_held = combo_held;
}
