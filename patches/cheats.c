#include "patches.h"
#include "input.h"
#include "cheats.h"

// Player resource block — four contiguous base-exe statics, always resident
// (no overlay validity guard needed). Addresses proven statically by the
// new-game initialiser func_8000B640_C240, which writes 100 -> +0x74 (money),
// 10 -> +0x6C/+0x70 (max/current HP) and 3 -> +0x78 (lives) through the same
// base. See docs/re-notes/goemon_cheats_re.md.
#define G_MAX_HP   (*(volatile s32*)0x8015C5E0)
#define G_CUR_HP   (*(volatile s32*)0x8015C5E4)
#define G_MONEY    (*(volatile s32*)0x8015C5E8)
#define G_LIVES    (*(volatile s32*)0x8015C5EC)

// Save-buffer mirror of money (live + 0x94). func_8000B718_C318 commits the
// live block here on area transitions; keeping it in sync stops a commit from
// rolling the lock back mid-frame.
#define G_MONEY_SAVE (*(volatile s32*)0x8015C67C)

// The game's own money cap. Do NOT lock higher: change_money range-checks with
// `lh $v0, 0x12($v1)` — big-endian, so it reads only the LOW signed halfword.
// Any value with bit 15 of the low halfword set reads as negative and the next
// change_money call takes the underflow branch and zeroes the player's money.
#define MONEY_MAX 9999

// Starting/normal life count, per the new-game initialiser and the floor
// enforced by func_8000B718_C318.
#define LIVES_NORMAL 3

// Same gameplay gate the analog camera uses: *0x8020CA2C is re-pointed every
// tick at a processed-stick record while in gameplay, so a plausible RDRAM
// pointer there means gameplay is live. func_800012FC_1EFC runs every frame
// including menus and the boot/new-game path, where the resource block is
// being zeroed and re-initialised — writing then would fight the initialiser.
#define CHEATS_MOVE_PTR ((u32*)0x8020CA2C)

static s32 cheats_in_gameplay(void) {
    u32 pp = *CHEATS_MOVE_PTR;
    return (pp >= 0x80000000u && pp < 0x80800000u);
}

void update_cheats(void) {
    if (!cheats_in_gameplay()) {
        return;
    }

    // Refill to the player's own max rather than a constant. This is
    // byte-for-byte what the game's refill func_8000B5BC_C1BC does, so it can't
    // desync the HUD (which reads the low halfword) or the heal clamp.
    // A large constant would be actively dangerous: nothing clamps this
    // variable, and change_health reads current HP as a SIGNED BYTE (`lb 0xF`),
    // so any value whose low byte is >= 0x80 reads negative and registers as
    // death.
    if (recomp_get_infinite_health_enabled()) {
        G_CUR_HP = G_MAX_HP;
    }

    // Purchases still work: the shop subtracts, and the next frame restores.
    if (recomp_get_infinite_money_enabled()) {
        G_MONEY = MONEY_MAX;
        G_MONEY_SAVE = MONEY_MAX;
    }

    if (recomp_get_infinite_lives_enabled()) {
        G_LIVES = LIVES_NORMAL;
    }
}
