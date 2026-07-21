#include "patches.h"
#include "input.h"
#include "attack_move.h"

// ============================================================================
// ATTACK WHILE WALKING / RUNNING
//
// Vanilla Goemon roots the player for the whole attack: the attack action
// states (action_id 0x58-0x5E) never run the movement pipeline, so world
// velocity (+0xC0/+0xC4/+0xC8) is held at zero for the entire swing even with
// the stick fully deflected (device-probed across 900+ frames).
//
// This feature re-injects movement during attacks by adding a per-frame
// displacement onto the authoritative player position node.
//
// DIRECTION: we do NOT reconstruct a heading from the camera basis. That was
// tried and it MOONWALKED — the reconstructed azimuth disagreed with the
// character's facing, so no sign flip could make it look right. Instead we
// sample the game's OWN world movement direction while the player is genuinely
// running (the position node's per-frame delta) and lunge along THAT during the
// attack. It is correct by construction: the player continues in the direction
// he was visibly running. The cost is that you cannot re-aim mid-swing — the
// lunge follows your last real run direction — which reads naturally.
//
// We write the position node directly rather than routing through the game's
// movement pipeline, which is where collision resolution lives. INFERENCE (not
// yet verified on device): that likely means the lunge is not collision-checked
// during the swing. Whether the player can actually enter geometry this way is
// UNCONFIRMED — no wall-clip has been observed. Normal collided movement resumes
// when the attack ends.
// ============================================================================

#if GOEMON_ATTACK_MOVE

// Print tuning data to logcat ([amove] ...). Off for release; flip to 1 to
// re-tune speed / ramp / hold on device.
#define ATTACK_MOVE_DIAG 0

// Peak per-frame world units at full stick deflection (mag ~0.7). Run is ~2.8;
// held under a run so it reads as a deliberate lunge. Scaled by deflection and
// by the ease envelope below.
#define ATTACK_MOVE_SPEED 1.2f

// Ease envelope: the target is approached, not snapped. env rises toward 1 while
// a move is requested (ease-IN) and falls toward 0 when the attack/input ends
// (ease-OUT glide, in the last direction). Per-frame low-pass rates.
#define ATTACK_MOVE_RAMP_IN  0.20f
#define ATTACK_MOVE_RAMP_OUT 0.28f
#define ATTACK_MOVE_ENV_EPS  0.004f

// Minimum squared per-frame delta (world units^2) to trust as "real movement"
// worth sampling a direction from. ~0.3 u/frame.
#define ATTACK_MOVE_TRACK_MIN 0.09f

// After the swing ends the game re-anchors the player toward the attack-start
// spot for a short recovery window (~16 frames observed). Keep injecting the
// full lunge for this many frames to cancel that pull-back — but only while he
// is still parked in recovery; the moment he starts a real run we release so we
// don't compound with the game's own locomotion.
#define ATTACK_MOVE_HOLD 16

// Processed-stick record: 24 bytes per player; +0xC = magnitude (f32). Read the
// STATIC base directly (the per-tick 0x8020CA2C alias is stale during attacks).
#define STICK_REC_BASE   0x800C7DB0
#define STICK_REC_STRIDE 24

// Covered attack states (device-identified via the action_id logger):
//   0x58-0x5E : melee pipe swing / jump-attack family
//   0x7C      : ryo (coin) throw
//   0x90      : bombs
// The pipe hookshot/extension (0x70-0x72) is deliberately EXCLUDED: it anchors
// the pipe to a world point, and sliding the player during it risks desyncing
// the grapple (same hazard class as the ladder character-swap failure).
#define ACT_ATK_LO 0x58
#define ACT_ATK_HI 0x5E
#define ACT_RYO    0x7C
#define ACT_BOMB   0x90

static inline s32 is_attack_state(u8 act) {
    return (act >= ACT_ATK_LO && act <= ACT_ATK_HI) ||
           act == ACT_RYO || act == ACT_BOMB;
}

static inline s32 in_ram(u32 p) {
    return p >= 0x80000000u && p < 0x80800000u;
}

// Add a planar world displacement to one position node (x at +0x8, z at +0x10;
// y at +0xC left alone — grounded attacks don't change height).
static inline void node_add(u32 node, f32 dx, f32 dz) {
    if (!in_ram(node)) {
        return;
    }
    *(f32*)(node + 0x8)  += dx;
    *(f32*)(node + 0x10) += dz;
}

// The game's own world movement direction, captured while the player actually
// moves. Correct by construction — no camera/heading reconstruction.
static f32 g_move_dx = 0.0f;
static f32 g_move_dz = 0.0f;
static s32 g_have_move = 0;

// Update the tracked direction from the head position node's per-frame delta.
// Runs every frame, but only SAMPLES a new direction during real locomotion
// states. Critically it must NOT sample during an attack: there the delta is
// our own injection plus any game pull-back, and reading it back would create a
// feedback loop — if the game nudges the player toward the attack-start spot,
// the tracker would flip to point at the origin and drive him there. Freezing
// the direction to the last genuine run keeps the lunge pointing the right way.
static void amove_track_dir(void) {
    u32 node = *(u32*)0x801FC60C;   // authoritative head position node
    if (!in_ram(node)) {
        return;
    }
    static f32 lx = 0.0f, lz = 0.0f;
    static s32 have_last = 0;

    // Area-transition reset (mirrors camera.c's map-id guard at 0x800C7AB2). The
    // frozen direction, and the last-position anchor used to sample it, belong to
    // the area they were captured in; a new area can be oriented differently in
    // world space, so a stale direction would lunge the wrong way on the first
    // post-transition attack. Drop the held direction so amove_target refuses
    // until a fresh genuine run re-latches it, and drop the anchor so the
    // transition's position discontinuity is not itself sampled as a bogus
    // direction on the next frame.
    static s32 last_map = -1;
    s32 map = (s32)*(volatile u16*)0x800C7AB2;
    if (map != last_map) {
        s32 prev = last_map;
        last_map = map;
        if (prev != -1) {
            g_have_move = 0;
            have_last = 0;
        }
    }

    f32 x = *(f32*)(node + 0x8);
    f32 z = *(f32*)(node + 0x10);

    PlayerTask *task = D_801FC604_5B8514;
    u8 act = task ? task->action_id : 0;
    s32 locomotion = (act >= 0x0C && act <= 0x11) || (act >= 0x17 && act <= 0x1C);

    if (have_last && locomotion) {
        f32 dx = x - lx, dz = z - lz;
        f32 d2 = dx * dx + dz * dz;
        if (d2 > ATTACK_MOVE_TRACK_MIN) {
            f32 inv = 1.0f / __builtin_sqrtf(d2);
            g_move_dx = dx * inv;
            g_move_dz = dz * inv;
            g_have_move = 1;
        }
    }
    lx = x;
    lz = z;
    have_last = 1;
}

// True while the player is in the post-attack recovery: not yet running and the
// game is not driving velocity, i.e. it is only re-anchoring him. When he starts
// a genuine run (or gains real velocity) this returns 0 so the hold releases.
static s32 amove_recovering(void) {
    PlayerTask *task = D_801FC604_5B8514;
    if (task == NULL) {
        return 0;
    }
    u8 act = task->action_id;
    if ((act >= 0x0C && act <= 0x11) || (act >= 0x17 && act <= 0x1C)) {
        return 0;   // real locomotion -> the game has him, don't fight it
    }
    f32 gvx = task->unknown_c0;
    f32 gvz = task->unknown_c8;
    if (gvx > 0.001f || gvx < -0.001f || gvz > 0.001f || gvz < -0.001f) {
        return 0;   // game is driving velocity -> release
    }
    return 1;
}

// Decide whether an attack-move is requested THIS frame and, if so, fill the
// full-strength (env == 1) per-frame target displacement in world x/z.
static s32 amove_target(f32* out_dx, f32* out_dz) {
    PlayerTask *task = D_801FC604_5B8514;   // g_player_1_task
    if (task == NULL) {
        return 0;
    }

    if (!is_attack_state(task->action_id)) {
        return 0;
    }

    // Skip jumps/air (also in 0x58-0x5E): the game drives velocity there itself.
    f32 gvx = task->unknown_c0;
    f32 gvz = task->unknown_c8;
    if (gvx > 0.001f || gvx < -0.001f || gvz > 0.001f || gvz < -0.001f) {
        return 0;
    }

    // Need input, and a known heading from recent real movement.
    if (!g_have_move) {
        return 0;
    }
    u8 pid = task->player_id;
    u8* rec = (u8*)(STICK_REC_BASE + STICK_REC_STRIDE * pid);
    f32 mag = *(f32*)(rec + 0xC);
    if (mag == 0.0f) {
        return 0;   // no input -> stand-still attack, untouched
    }

    // Lunge along the direction the player was last genuinely running, at our
    // own speed, scaled by current stick deflection.
    f32 scale = ATTACK_MOVE_SPEED * mag;
    *out_dx = g_move_dx * scale;
    *out_dz = g_move_dz * scale;
    return 1;
}

void attack_move_tick(void) {
    // Whole feature gates on the "Attack While Moving" setting (defaults Off).
    // When Off it is entirely inert — no tracking, no injection.
    if (!recomp_get_attack_while_moving_enabled()) {
        return;
    }

    amove_track_dir();   // keep the tracked world heading fresh every frame

    // Smoothed ease envelope + last full-strength target, persistent so the
    // glide-out continues after the attack/input ends.
    static f32 env = 0.0f;
    static f32 tdx = 0.0f, tdz = 0.0f;
    static s32 hold = 0;

    f32 dx, dz;
    s32 target = amove_target(&dx, &dz);
    s32 active;

    if (target) {
        tdx = dx;
        tdz = dz;
        hold = ATTACK_MOVE_HOLD;   // arm the recovery hold
        active = 1;
    } else if (hold > 0 && amove_recovering()) {
        hold--;                    // keep opposing the re-anchor at full lunge
        active = 1;
    } else {
        hold = 0;
        active = 0;
    }

    if (active) {
        env += (1.0f - env) * ATTACK_MOVE_RAMP_IN;    // ease-in
    } else {
        env += (0.0f - env) * ATTACK_MOVE_RAMP_OUT;   // ease-out glide
    }

    if (env < ATTACK_MOVE_ENV_EPS) {
        env = 0.0f;
        return;
    }

    f32 adx = tdx * env;
    f32 adz = tdz * env;

    // Move ONLY the confirmed authoritative head position node (proven vec3f in
    // docs/re-notes/goemon_player_pos.md). Poking the speculative node chain
    // crashed the renderer earlier (a pointer field at +0x8), so touch only it.
    u32 head = *(u32*)0x801FC60C;
    node_add(head, adx, adz);

#if ATTACK_MOVE_DIAG
    static s32 n = 0;
    if ((n++ & 3) == 0) {
        // Absolute head pos too: if it drifts back toward a fixed anchor while
        // we keep adding adx/adz, the game is restoring position independently.
        f32 px = in_ram(head) ? *(f32*)(head + 0x8)  : 0.0f;
        f32 pz = in_ram(head) ? *(f32*)(head + 0x10) : 0.0f;
        recomp_printf("[amove] act%d env=%d dir=(%d,%d) d=(%d,%d) pos=(%d,%d)\n",
                      active, (s32)(env * 1000.0f),
                      (s32)(g_move_dx * 1000.0f), (s32)(g_move_dz * 1000.0f),
                      (s32)(adx * 1000.0f), (s32)(adz * 1000.0f),
                      (s32)px, (s32)pz);
    }
#endif
}

#endif // GOEMON_ATTACK_MOVE
