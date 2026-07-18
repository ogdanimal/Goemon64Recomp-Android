#include "patches.h"
#include "input.h"

// ============================================================================
// Character swapping while moving.
//
// Vanilla gate: func_801DD3C4_5992D4 permits a C-Down character swap only when
// PlayerTask.action_id (+0xCC) is one of {0, 1, 0x42} — the three IDLE STANCES.
// It is not a velocity or grounded check; it is literally "you are playing an
// idle animation". Those three ids are exactly what the swap state machine
// writes back on completion, so the swap is a closed idle->idle loop that never
// saves or restores the prior action.
//
// This patch widens ONLY that whitelist, and only when the setting is enabled,
// to include the grounded locomotion ladder. Conditions B/C/D are reproduced
// verbatim from the recompiled original (RecompiledFuncs/funcs_26.c) — they
// guard carried/mounted objects, a busy camera, and an Ebisumaru special case,
// and are shared with the sibling C-Up action.
//
// The permitted set is a WHITELIST, deliberately. On-device testing found that
// swapping on a LADDER leaves the player floating/T-posed and corrupts the
// ladder object itself (it stays unusable until the area is reloaded; one case
// required a force-close). A state that breaks that badly means unknown states
// must default to REFUSED — a blacklist would leave every untested state
// (cutscenes, minigames, vehicles, boss sequences) allowed by default.
//
// Every id below was exercised on device and observed to swap cleanly; the
// swap cancels the in-progress action and drops to a standstill, and control
// resumes normally. See docs/re-notes/goemon_character_swap_re.md.
//
// NOTE: the swap itself still costs ~0.93s of locked input (measured), because
// func_801DC9C8_5988D8 DMAs and decompresses the character's graphics file from
// ROM over the shared per-player buffer. This setting only removes the
// requirement to come to a stop FIRST; it cannot make the swap seamless.
// ============================================================================

// Print gate decisions to logcat ([swap] ALLOWED / BLOCKED + reason). Off by
// default; set to 1 when debugging why a swap is or isn't being permitted.
#define GOEMON_SWAP_DIAG 0

// Grounded movement cycle. 0x0C-0x11 were correlated against stick deflection
// (0x0C creep at 1-18%, 0x0D slow walk at 19-27%, 0x0E walk at 29-50%,
// 0x0F run at 56-60%, 0x10 decelerating, 0x11 stopping). 0x17-0x1C are the
// rest of the same gait cycle: the state log shows them cycling
// 0x17->0x19->0x1A->0x1C->0x0D->0x0F continuously while running.
#define ACT_MOVE_LO      0x0C
#define ACT_MOVE_HI      0x11
#define ACT_GAIT_LO      0x17
#define ACT_GAIT_HI      0x1C

// Airborne/attack family. 0x58, 0x59, 0x5A, 0x5D and 0x5E were each verified
// by swapping out of them on device.
#define ACT_AIR_LO       0x58
#define ACT_AIR_HI       0x5E

// Individually verified action states (identity not established, but each was
// swapped out of cleanly).
#define ACT_MISC_1       0x40
#define ACT_MISC_2       0x43
#define ACT_MISC_3       0x71
#define ACT_MISC_4       0xA2

// ---- Known-BAD, never permitted ----
// Ladder climb, alternating while ascending/descending. Swapping here is the
// reproduced failure: floating/T-pose plus a ladder left broken until the area
// reloads. Listed for documentation; the whitelist already excludes them.
#define ACT_LADDER_A     0x35
#define ACT_LADDER_B     0x38
// Swap already in progress. Permitting this allows a second character load to
// start over the shared graphics buffer while one is still settling. It was
// observed to work, but it is re-entrancy over the buffer being written and
// there is nothing to gain by allowing it.
#define ACT_SWAPPING     0xBA

// Processed-stick record base; 24 bytes per player. +0x2 = HELD, +0x4 = PRESSED
// (rising edge, computed at 0x80004CD8). C-Down = 0x4.
#define STICK_REC_BASE   0x800C7DB0
#define STICK_REC_STRIDE 24
#define BTN_C_DOWN       0x4

// Camera task pointer, read by condition C (lui 0x8020 / lw -0x39DC).
#define G_CAMERA_TASK    (*(u32*)0x801FC624)

static s32 is_swap_safe(u8 act) {
    // Ladder states are the one reproduced failure — refuse explicitly, ahead
    // of any range check, so a future range widening cannot re-admit them.
    if (act == ACT_LADDER_A || act == ACT_LADDER_B || act == ACT_SWAPPING) {
        return 0;
    }

    if (act >= ACT_MOVE_LO && act <= ACT_MOVE_HI) return 1;
    if (act >= ACT_GAIT_LO && act <= ACT_GAIT_HI) return 1;
    if (act >= ACT_AIR_LO  && act <= ACT_AIR_HI)  return 1;

    return act == ACT_MISC_1 || act == ACT_MISC_2 ||
           act == ACT_MISC_3 || act == ACT_MISC_4;
}

// @recomp Widen the character-swap action-state whitelist to include grounded
// locomotion, behind the "Swap While Moving" setting. Conditions B/C/D are
// unchanged from the original.
RECOMP_PATCH s32 func_801DD3C4_5992D4(u8* task) {
    u8 act = *(u8*)(task + 0xCC);
    s32 enabled = recomp_get_swap_while_moving_enabled();
    s32 idle_ok = (act < 2) || (act == 0x42);
    s32 moving_ok = 0;

    if (!idle_ok) {
        if (!enabled || !is_swap_safe(act)) {
#if GOEMON_SWAP_DIAG
            static s32 last_blocked = -1;
            if (act != last_blocked) {
                recomp_printf("[swap] BLOCKED action_id=0x%02X (enabled=%d)\n",
                              act, enabled);
                last_blocked = act;
            }
#endif
            return 0;
        }
        moving_ok = 1;
    }

    // Edge-trigger the swap while this setting is on. The dispatcher
    // func_801DCE10_598D20 tests C-Down with `held & 0x4` (LEVEL, not edge) —
    // vanilla gets away with it because the ~0.93s input lock swallows the
    // repeats, but once a swap can start mid-run the level trigger would cycle
    // characters continuously for as long as the button is held. Requiring the
    // rising edge means one press = one swap. Only applied when the setting is
    // on, so vanilla behaviour is untouched when it is off.
    if (enabled) {
        u8 player_id = *(u8*)(task + 0x90);
        u8* rec = (u8*)(STICK_REC_BASE + STICK_REC_STRIDE * player_id);
        u16 pressed = *(u16*)(rec + 0x4);

        if (!(pressed & BTN_C_DOWN)) {
            return 0;
        }
    }

    // ---- CONDITION B: carried/mounted object must be in state 2 ----
    {
        u32 o = *(u32*)(task + 0x38);
        if (o != 0 && *(u8*)(o + 0x4C) != 2) {
#if GOEMON_SWAP_DIAG
            recomp_printf("[swap] BLOCKED attached-object state=%d\n",
                          *(u8*)(o + 0x4C));
#endif
            return 0;
        }
    }

    // ---- CONDITION C: camera not busy ----
    if (*(u8*)(G_CAMERA_TASK + 0xD2) != 0) {
#if GOEMON_SWAP_DIAG
        recomp_printf("[swap] BLOCKED camera busy\n");
#endif
        return 0;
    }

    // ---- CONDITION D: character 1 special case ----
    if (*(u8*)(task + 0x60) == 1) {
        u32 player = *(u32*)(task + 0x5C);
        if (*(u16*)(player + 0x86) == 0xFFFF) {
#if GOEMON_SWAP_DIAG
            recomp_printf("[swap] BLOCKED character-1 special case\n");
#endif
            return 0;
        }
    }

#if GOEMON_SWAP_DIAG
    recomp_printf("[swap] ALLOWED action_id=0x%02X moving=%d\n", act, moving_ok);
#endif
    return 1;
}
