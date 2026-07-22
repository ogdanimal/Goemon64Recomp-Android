#include "patches.h"
#include "autosave.h"
#include "misc_funcs.h"
#include "input.h"

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
// Kept only as a diagnostic -- it usefully confirms the L/Z part of the combo is
// reaching the game's own button word. (As of 2026-07-20 the combo's R part is
// read from the physical trigger, not this word, and N64 R is masked out of it
// entirely while analog cam is on -- so do not expect 0x0010 here in that mode.)
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
// Save-data-settled check.
//
// The port of Zelda64Recomp's stability window: require the save-relevant state
// to be unchanged for a short span before committing, so a write never lands
// mid-transaction (an item being consumed, a flag being applied). That would
// produce a technically valid save recording a half-applied state.
//
// This protects the AUTOSAVE ITSELF. The .manual.bak rollback point protects
// everything around it. They are independent, and both gate defaulting the
// timer to On.
//
// WHAT IS WATCHED, AND WHY IT IS TWO BUFFERS
// ------------------------------------------
// Watching gamedata alone would be wrong. The marshal func_8000B718_C318 is
// what copies the live player block into gamedata+0x64, and it runs at save
// time -- so between saves gamedata's copy of HP, ryo and lives is STALE. A
// check watching only gamedata would be blind to the player taking damage or
// spending money, which is most of what "mid-transaction" means here.
//
// Watching the live block alone would be equally wrong in the other direction:
// it holds no event flags or progress bits.
//
// So both, as four contiguous ranges:
//
//   A  0x8015C5D8 + 0x30   the live block, entire. Confirmed layout: max HP
//                          +0x08, current HP +0x0C, ryo +0x10, lives +0x14,
//                          counter +0x18 (mirrors gamedata +0x6C/+0x70/+0x74/
//                          +0x78/+0x7C). Every writer is event-driven.
//   B  gamedata +0x000     event-flag bitfield (800 flags)
//   C  gamedata +0x094     progress blocks, second bitfield, room id and
//                          spawn point
//   D  gamedata +0x268     unlock/status tables and gauges
//
// DELIBERATE EXCLUSIONS -- each of these is a trap, not an omission:
//
//   gamedata +0x064..+0x093  The stale mirror of range A. Excluding it is not
//                            just redundancy: it changes ONLY when a save runs,
//                            so watching it would inject a spurious "unsettled"
//                            event at exactly the moment a save is being
//                            attempted.
//   gamedata +0x264..+0x267  Play time. Written live into gamedata, +1 every 60
//                            frames (it is seconds, not frames). It would not
//                            deadlock a 10-frame window, but it is pure noise.
//   gamedata +0x300..+0x303  Footer/CRC region, touched at save time.
//   the shadow at 0x8015C910 Written only at save/load; adds nothing.
//
// Player world position is NOT in the payload (no float store resolves into
// either buffer; the only positional fields are 16-bit SPAWN coordinates read
// from static tables). So walking around does not prevent settling -- which
// matters, because a check that only settled while standing still would refuse
// almost every save.
//
// Room transitions DO register as unsettled, via +0x204..+0x211 inside range C.
// That is wanted: a transition is precisely when a save should not land.
// ---------------------------------------------------------------------------

// Frames the watched state must hold still before a save may commit. Ported
// from Zelda64Recomp's ~10-frame window.
//
// Validated on device 2026-07-19 by temporarily widening this to 120 so the
// refusal path could be hit by hand. Two refusals reported 98/120 and 32/120
// frames, matching the wall-clock gaps since the preceding change (1.634s and
// 0.535s) at 60Hz to within a millisecond -- so the counter tracks real frames,
// and the check is not merely printing plausible output.
#define SETTLE_FRAMES 10
#define SETTLE_RANGE_COUNT 4

static const struct {
    u32 addr;
    u32 size;
    const char* name;
} settle_ranges[SETTLE_RANGE_COUNT] = {
    { 0x8015C5D8, 0x030, "live"     },  // A: HP / ryo / lives
    { 0x8015C608, 0x064, "flags"    },  // B: gamedata +0x000
    { 0x8015C69C, 0x1D0, "progress" },  // C: gamedata +0x094 .. +0x263
    { 0x8015C870, 0x098, "unlocks"  },  // D: gamedata +0x268 .. +0x2FF
};

static u32 settle_hash[SETTLE_RANGE_COUNT];
static s32 settle_frames;
static s32 settle_primed;
static u32 settle_changed_mask;

// FNV-1a. Cheap enough to run over ~0x2FC bytes every frame, and we only need
// change detection, not collision resistance.
static u32 settle_hash_range(u32 addr, u32 size) {
    const volatile u8* p = (const volatile u8*)addr;
    u32 h = 0x811C9DC5u;
    u32 i;

    for (i = 0; i < size; i++) {
        h ^= (u32)p[i];
        h *= 0x01000193u;
    }
    return h;
}

// Must be called EVERY frame, not lazily at trigger time -- the frame counter
// is meaningless otherwise.
static void update_settle_state(void) {
    u32 changed = 0;
    s32 i;

    for (i = 0; i < SETTLE_RANGE_COUNT; i++) {
        u32 h = settle_hash_range(settle_ranges[i].addr, settle_ranges[i].size);
        if (h != settle_hash[i]) {
            changed |= (1u << i);
        }
        settle_hash[i] = h;
    }

    // First pass after enabling only seeds the hashes; everything looks
    // "changed" against zeroed state, which is not a real transaction.
    if (!settle_primed) {
        settle_primed = 1;
        settle_frames = 0;
        settle_changed_mask = 0;
        return;
    }

    if (changed != 0) {
        settle_frames = 0;
        settle_changed_mask = changed;
    } else if (settle_frames < SETTLE_FRAMES) {
        settle_frames++;
    }
}

static s32 autosave_is_settled(void) {
    return settle_frames >= SETTLE_FRAMES;
}

// ---------------------------------------------------------------------------
// The save itself.
// ---------------------------------------------------------------------------

// Slot cursor for logging ONLY. Returns -1 when the save context pointer is
// null or points outside RDRAM, so a status line during a failed save reports
// a sentinel instead of dereferencing a masked/garbage pointer and printing a
// bogus slot (the save path itself guards this at goemon_save_now_inner; the
// log sites must not lie when it bails).
static s32 autosave_log_slot(void) {
    u32 ctx = G_SAVE_CTX_PTR;
    if (ctx < 0x80000000u || ctx >= 0x80800000u) {
        return -1;
    }
    return (s32)*(volatile u32*)(ctx + G_SLOT_OFFSET);
}

// The body. Do NOT call this directly -- goemon_save_now() below wraps it with
// the save-rollback bracket, and every pak write this makes must happen inside
// that bracket.
static s32 goemon_save_now_inner(void) {
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

// Wraps the save in the rollback bracket, telling the host that every pak write
// between these two calls is the autosave's own rather than a deliberate,
// player-initiated save. The host maintains a `.manual.bak` rollback point by
// observing pak writes it did NOT see bracketed -- see src/game/save_rollback.cpp
// and docs/autosave.md.
//
// TWO THINGS THIS STRUCTURE IS DELIBERATELY BUYING, both easy to undo by
// "simplifying" it back into the caller:
//
//  1. The bracket covers the WHOLE body, not just the slot write. The save also
//     pushes a 0x100-byte header block through the pak (step 10), and an
//     unbracketed header write would arm the host's one-shot and copy autosave
//     content into `.manual.bak` -- exactly the state it exists to roll back
//     from. C has no RAII, so a single wrapper is what keeps every early return
//     (bad slot, probe failure, allocation failure) balanced.
//  2. The bracket lives inside goemon_save_now(), not at the call site, so the
//     timer -- when it lands -- inherits it instead of having to remember it.
s32 goemon_save_now(void) {
    s32 result;

    recomp_set_autosave_in_progress(1);
    result = goemon_save_now_inner();
    recomp_set_autosave_in_progress(0);

    return result;
}

// ---------------------------------------------------------------------------
// The timer.
//
// Confirmed to be the right trigger shape rather than a fallback: a whole-ROM
// scan established that Goemon has NO automatic commit points -- every one of
// the 12 pak-write sites is behind an explicit player confirmation. There is
// nothing to piggyback on, so a timer is not passing over a better design.
//
// Both gating preconditions are met and device-verified: the .manual.bak
// rollback point protects everything around the autosave, and the settled check
// above protects the autosave itself.
//
// THREE BEHAVIOURS WORTH KNOWING, none of them incidental:
//
//  1. An elapsed interval does NOT consume the save. If the gate or the settled
//     check refuses, the timer keeps retrying every frame rather than skipping
//     to the next period. Otherwise an interval that happened to elapse during
//     a cutscene would silently lose that save entirely.
//  2. Nothing is written if nothing changed. The settled hashes are snapshotted
//     at each save; if they still match, the save is skipped. Standing idle
//     therefore produces no writes at all -- which matters because every flush
//     rotates .bak, so a timer that wrote unconditionally would churn the
//     runtime's backup for no benefit.
//  3. A failed save resets the timer anyway. Otherwise a persistently failing
//     save (a pak error, say) would retry every single frame.
// ---------------------------------------------------------------------------

// 2 minutes. Wrap-safe: recomp_time_us() is u32 microseconds and wraps about
// every 71 minutes, but the unsigned subtraction below is correct across a wrap
// for any interval short of the full period.
// 2 minutes.
//
// Validated on device 2026-07-19 by temporarily shortening this to 20s. The
// full cycle was observed: fire -> suppress while idle -> re-arm on a state
// change -> suppress again, with successive intervals measuring 20.000s and
// 20.033s (the 33ms is two frames, i.e. the per-frame check granularity).
#define AUTOSAVE_INTERVAL_US (120u * 1000u * 1000u)

static u32 autosave_last_us;
static s32 autosave_timer_primed;
static u32 autosave_saved_hash[SETTLE_RANGE_COUNT];
static s32 autosave_saved_hash_valid;

static s32 autosave_state_changed_since_save(void) {
    s32 i;

    if (!autosave_saved_hash_valid) {
        return true;
    }
    for (i = 0; i < SETTLE_RANGE_COUNT; i++) {
        if (settle_hash[i] != autosave_saved_hash[i]) {
            return true;
        }
    }
    return false;
}

// Call after any save this feature makes, manual or timed, so a timed save does
// not land moments after the player saved by hand.
static void autosave_note_committed(u32 now_us, s32 status) {
    s32 i;

    autosave_last_us = now_us;

    if (status == 0) {
        for (i = 0; i < SETTLE_RANGE_COUNT; i++) {
            autosave_saved_hash[i] = settle_hash[i];
        }
        autosave_saved_hash_valid = 1;

        // Both save paths (timed and the manual combo) funnel through here, and
        // only a status of 0 actually committed anything -- so this is the one
        // place the "Saved" toast can be raised without it ever lying.
        recomp_notify_saved();
    }
}

// ---------------------------------------------------------------------------
// Per-frame poll.
// ---------------------------------------------------------------------------

// Manual save trigger: L + R + Z (physically LB + RT + LT on a modern pad).
//
// Was L + R + D-Up. The reworked controller map binds the physical D-pad to the
// C-buttons, so N64 D-Up is no longer producible -- the old combo became
// unreachable. Z is the replacement third button because it is one of only three
// N64 inputs that are harmless to hold simultaneously: L (natively unused), Z
// (crouch), and R (camera-hold, only when NO C-button accompanies it). Every
// other candidate fires a side effect while the combo is held -- any C-button
// casts magic / swaps weapon / changes character / opens the map, R+C triggers
// native camera zoom, and Start opens the pause menu (the gate already
// special-cases 0x1000=Start). Three shoulders/triggers is also a deliberate
// grip that will not be hit by accident during play.
//
// The R part is read from the PHYSICAL right trigger (recomp_get_camera_zoom_held)
// rather than the N64 R bit, because the analog camera masks N64 R out of the
// button word while it is on (R is its zoom modifier). Reading the trigger
// directly makes the combo behave identically whether analog cam is on or off. L
// and Z are unaffected by that mask and stay in the N64 button word.
#define AUTOSAVE_LZ_MASK (L_TRIG | Z_TRIG)

void update_autosave(void) {
    static s32 combo_was_held = 0;
    u16 held;
    s32 combo_held;
    u32 now_us;
    s32 is_safe;

    if (!recomp_get_autosave_enabled()) {
        combo_was_held = 0;
        settle_primed = 0;
        autosave_timer_primed = 0;
        return;
    }

    // Unconditionally, every frame -- see update_settle_state().
    update_settle_state();

    now_us = recomp_time_us();
    if (!autosave_timer_primed) {
        autosave_last_us = now_us;
        autosave_timer_primed = 1;
    }

    is_safe = autosave_is_safe();

    held = D_8008CCC0_8D8C0.controller[0].button_held_down;
    combo_held = ((held & AUTOSAVE_LZ_MASK) == AUTOSAVE_LZ_MASK) &&
                 recomp_get_camera_zoom_held();

    // Edge-triggered, so holding the combo saves once rather than every frame.
    if (combo_held && !combo_was_held) {
        if (is_safe && !autosave_is_settled()) {
            // Named ranges, not just a count. A whitelist containing one
            // continuously-volatile field would refuse EVERY save while looking
            // exactly like a gate working correctly -- the same failure shape as
            // the inverted phase guard that refused every save until the
            // diagnostic exposed it. Printing which range last moved makes a bad
            // whitelist visible in one test cycle instead of looking correct.
            recomp_printf("[autosave] refused: save data not settled "
                          "(%d/%d frames | last change:%s%s%s%s)\n",
                          settle_frames, SETTLE_FRAMES,
                          (settle_changed_mask & 1) ? " live"     : "",
                          (settle_changed_mask & 2) ? " flags"    : "",
                          (settle_changed_mask & 4) ? " progress" : "",
                          (settle_changed_mask & 8) ? " unlocks"  : "");
        } else if (is_safe) {
            s32 status = goemon_save_now();

            // Feeds the timer too, so a timed save cannot land moments after
            // the player just saved by hand.
            autosave_note_committed(now_us, status);

            if (status == AUTOSAVE_ERR_BAD_SLOT) {
                recomp_printf("[autosave] refused: no valid save slot selected "
                              "(cursor %d, need < %d)\n",
                              autosave_log_slot(),
                              SAVE_SLOT_COUNT);
            } else {
                recomp_printf("[autosave] manual save -> status %d (slot %d)\n",
                              status,
                              autosave_log_slot());
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

    // The timer. Deliberately does NOT consume the interval when it cannot
    // save -- see the header above; it retries next frame instead.
    if ((u32)(now_us - autosave_last_us) >= AUTOSAVE_INTERVAL_US) {
        // Don't let a 2-minute boundary commit while the recomp settings menu is
        // open: the game ticks with input zeroed underneath the overlay, so a
        // timed save there is coherent but against the gate's intent (N14). The
        // manual combo isn't gated here because its L/Z reads already come from
        // the zeroed game input word while the menu is up.
        if (is_safe && !recomp_is_config_menu_open() &&
            autosave_is_settled() && autosave_state_changed_since_save()) {
            s32 status = goemon_save_now();

            autosave_note_committed(now_us, status);
            recomp_printf("[autosave] timed save -> status %d (slot %d)\n",
                          status,
                          autosave_log_slot());
        }
    }
}
