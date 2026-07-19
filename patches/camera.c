#include "patches.h"
#include "input.h"
#include "camera.h"

// @recomp Analog (right-stick) camera control.
//
// ARCHITECTURE — consumer-side hooks, which is the correct design here, not a
// workaround: the game has NO per-frame camera writer. The default Camera is
// static BSS (0x8020CBF0) and poses are set only on zone-trigger CUTS (choke
// point func_8001C3E0), so there is nothing to "drive" natively. Instead both
// consumers of the camera are intercepted:
//   1. Render  — apply_analog_camera() rotates a private Camera copy before
//      func_80017D8C_1898C builds the view (guPerspective + guLookAtHilite).
//   2. Movement — RECOMP_PATCH of func_801CE3F0_58A300 swaps the camera node's
//      Camera pointer to the same rotated copy across the func_801CE4D0 call.
//      That resolver derives its movement basis from eye−look_at of the LIVE
//      Camera, so the swap makes walking match the rendered view natively.
//      (The old host left-stick counter-rotation hack is off — pass 0.)
//
// The camera is an ABSOLUTE override, not an offset added to the live camera:
// see the g_acam_dir_* comment below for why that distinction fixes drift. We
// own the camera's AZIMUTH only; its distance and height stay the game's.
//
// Settings consumed from the menu: analog cam on/off, per-axis invert, per-axis
// sensitivity, and R3 to hand the camera back to the game.

// Processed analog-stick record pointer: a valid pointer only during
// gameplay (0 on the title screen and system menus) — used as the gameplay
// gate. (RE update: this is NOT a movement/heap object — func_801CC4C0
// re-points it every tick at a static 0x18-byte stick record in the table
// at 0x800C7DB0: +0xC magnitude, +0x10/+0x14 planar components.)
#define ACAM_MOVE_PTR ((u32*)0x8020CA2C)

// Current map id. The engine's area-change commit (func_8000B364) copies this
// halfword to the previous-map slot 0x800C7ABC, then installs the pending
// destination from 0x800C7CA0 — so a change here IS an area transition, one
// cheap 2-byte read with no pointer chase. Map ids are per-room (0..0x26C, 621
// of them across only 14 stages), which is the granularity a house interior
// needs. Three independent addressing forms in the binary resolve here.
//
// NOTE: gamedata+0x200 looks like an area id but is NOT — it indexes the
// continue/save-point table at 0x8005BA30 and only moves when you save.
#define ACAM_MAP_ID ((volatile u16*)0x800C7AB2)

// Accumulated analog-camera yaw and pitch, s16 binary angles
// (0x10000 = full turn).
s16 g_analog_cam_yaw = 0;
s16 g_analog_cam_pitch = 0;

// Absolute-override state (v14). PROBLEM this fixes: rotating the LIVE game
// camera each frame inherits Goemon's follow-cam, which swings its own azimuth
// behind your movement — so an added offset visibly drifts back while walking.
// FIX (Zelda64Recomp update_analog_camera_params model): the first frame the
// camera is engaged, snapshot the eye→look_at offset; from then on rotate THAT
// fixed offset by the accumulated yaw/pitch and re-anchor it to the LIVE
// look_at every frame. look_at still tracks the player, so the camera follows
// you around, but its azimuth is ours to hold — the game's follow-cam no longer
// bleeds in. Captured once per engagement; re-snapped on disengage so
// re-engaging never jumps.
// Only the horizontal DIRECTION is captured — a unit vector. Radius and height
// are re-read from the live camera every frame and left entirely to the game.
//
// WHY NOT capture the whole offset (which is what v14 did): the offset encodes
// the framing DISTANCE, and the game's distance is dynamic. On entering an area
// the camera starts very close to the character and eases out to its normal
// distance over ~2s (device recording, 2026-07-19: at 1.5s after the load
// Goemon fills the frame; by 4.0s the camera has pulled back on its own with no
// input) — and movement affects it too. Capturing during that window froze the
// camera at "very close" for the whole area, fixable only with R3. Deferring the
// capture until the pose settles would only have moved the guess around, since
// there is no single instant that is reliably "the" framing.
//
// Taking radius/height live cannot reintroduce the v14 drift bug: that was the
// game's follow-cam swinging its AZIMUTH behind movement, and azimuth is the one
// quantity we never read back. Radius and height are azimuth-independent
// scalars. And since the game has no per-frame camera writer (see the RE notes),
// they are constant except at cuts and eases — so there is no jitter to inherit.
static s32 g_acam_captured = 0;
static f32 g_acam_dir_x = 0.0f;
static f32 g_acam_dir_z = 0.0f;

// Last map id seen, for the area-transition reset. -1 = nothing seen yet, so
// the first gameplay frame is not reported as a transition.
static s32 g_acam_last_map = -1;

// v12 feel: TIME-BASED rates (binang per second at full stick tilt), so the
// rotation speed no longer depends on how many times per rendered frame the
// update hook runs (known to vary — the old per-call 0x200 made the feel
// call-rate-dependent and touchy). ~0x5800/s ~= 124 deg/s yaw.
#define ACAM_YAW_RATE_PER_S   0x5800
#define ACAM_PITCH_RATE_PER_S 0x2C00

// Pitch clamp: positive pitch raises the eye (looking down on the player).
// Keep the down-look generous and the up-look modest (camera under the
// floor looks broken long before the math does).
#define ACAM_PITCH_MAX 0x2000
#define ACAM_PITCH_MIN (-0x1000)

// Absolute eye-elevation clamp (v13): the TOTAL angle of eye−pivot above
// the horizontal is limited to [−10°, +55°] whenever analog pitch is
// applied. The game's own camera often already looks down ~30°, so a
// relative clamp alone can reach near-vertical, where guLookAt's fixed
// (sin r, cos r, 0) up-vector degenerates (the wild rolled views on v12
// footage). sin/cos pairs for the two limits:
#define ACAM_ELEV_MAX_S 0.8192f  /* sin +55deg */
#define ACAM_ELEV_MAX_C 0.5736f  /* cos +55deg */
#define ACAM_ELEV_MIN_S 0.1736f  /* |sin -10deg| */
#define ACAM_ELEV_MIN_C 0.9848f  /* cos -10deg */

// Clamp for a single time step; also swallows the first call and any pause
// (dt spikes must not whip the camera).
#define ACAM_MAX_DT_US 50000u

// math_sin (func_80003E10_4A10) returns a true unit sine in [-1,1], but its
// angle convention is a FULL TURN = 0x400 (1024), NOT the 0x10000 binang system
// our accumulators use. Proof: widescreen.c masks its argument `& 0x3FF` before
// calling it. Our yaw/pitch are s16 0x10000 binang, so scale down by 64 (>>6)
// into the 0x400 range; a quarter turn (for cosine) is 0x100 there, not 0x4000.
//
// The previous code passed the raw 0x10000-scale angle and added 0x4000 for
// cosine — 64x too large AND a whole number of 0x400 periods, so sin and cos
// aliased to the SAME table entry (s==c, matrix non-orthonormal). That silently
// rescaled the eye radius every frame instead of rotating it: the camera swung
// into and out of the player (rOut collapsing to 0) rather than orbiting.
static f32 acam_sin(s32 angle) { return func_80003E10_4A10((angle >> 6) & 0x3FF); }
static f32 acam_cos(s32 angle) { return func_80003E10_4A10(((angle >> 6) + 0x100) & 0x3FF); }

// Camera-relative movement resolver core (o32: task, out=task+0x74, planar
// stick x/z, speed on stack). Its basis = normalize(Camera.eye − look_at)
// read via *(*(*(task+0x64)+0x18)+0x2C) & 0x8FFFFFFE.
s32 func_801CE4D0_58A3E0(void* task, f32* out, f32 x, f32 z, f32 speed);

static s32 acam_in_gameplay(void) {
    u32 pp = *ACAM_MOVE_PTR;
    return (pp >= 0x80000000u && pp < 0x80800000u);
}

// The object dispatcher submits SEVERAL cameras per frame — the gameplay
// camera AND a default-constructed UI/system camera that is bit-exactly
// eye=(0,0,100), at=(0,0,0). Rotating the UI camera corrupts HUD/overlay
// passes, so it must be filtered out.
static s32 acam_is_gameplay_camera(Camera* cam) {
    if (cam->position.x == 0.0f && cam->position.y == 0.0f &&
        cam->position.z == 100.0f &&
        cam->look_at.x == 0.0f && cam->look_at.y == 0.0f &&
        cam->look_at.z == 0.0f) {
        return 0;
    }
    return 1;
}

void update_analog_camera(void) {
    s32 enabled = recomp_get_analog_cam_enabled();

    // Silence the right stick's C-button mapping only while active.
    recomp_set_right_analog_suppressed(enabled);

    // Area-transition reset. The held azimuth (g_acam_dir_* plus accumulated
    // yaw) belongs to the area it was captured in; each area sets its own
    // intended camera direction on entry, and without this the old area's
    // heading survived the transition and overrode it — fixable only by pressing
    // R3 every single time. Clearing the capture makes the next frame
    // re-snapshot from the live camera, which is exactly what R3 does by hand.
    //
    // Distance and height need no reset: they are read live every frame now.
    //
    // Yaw/pitch must be zeroed with it: they are applied ON TOP of a freshly
    // captured offset, so keeping stale rotation would re-snapshot the new
    // area's framing and then immediately swing away from it.
    //
    // Tracked unconditionally (above the enabled/gameplay gate) so the id never
    // goes stale while the feature is off or during a load.
    s32 map = (s32)*ACAM_MAP_ID;
    if (map != g_acam_last_map) {
        s32 prev = g_acam_last_map;
        g_acam_last_map = map;
        if (prev != -1) {
            g_analog_cam_yaw = 0;
            g_analog_cam_pitch = 0;
            g_acam_captured = 0;
        }
    }

    if (!enabled || !acam_in_gameplay()) {
        g_analog_cam_yaw = 0;
        g_analog_cam_pitch = 0;
        g_acam_captured = 0;  // re-snapshot the offset on the next engagement
        recomp_set_analog_cam_yaw(0);
        return;
    }

    // Time step for rate-based accumulation (per-call dt sums correctly no
    // matter how often this hook runs per rendered frame).
    static u32 acam_last_us = 0;
    u32 now_us = recomp_time_us();
    u32 dt_us = now_us - acam_last_us;
    acam_last_us = now_us;
    if (dt_us > ACAM_MAX_DT_US) {
        dt_us = 0;
    }
    f32 dt = (f32)dt_us * (1.0f / 1000000.0f);

    // R3 (right-stick click) = recenter: hand the camera back to the game.
    // Dropping the capture makes apply_analog_camera pass the live camera
    // through untouched, so Goemon's own follow-cam resumes and trails behind
    // the player again; the next right-stick nudge re-engages with a fresh
    // snapshot. Edge-detected because this hook can run several times per
    // rendered frame (a level read would re-fire every call while held).
    static s32 acam_prev_recenter = 0;
    s32 recenter = recomp_get_camera_recenter_pressed();
    if (recenter && !acam_prev_recenter) {
        g_analog_cam_yaw = 0;
        g_analog_cam_pitch = 0;
        g_acam_captured = 0;
    }
    acam_prev_recenter = recenter;

    float input_x, input_y;
    recomp_get_camera_inputs(&input_x, &input_y);

    // Per-axis invert (menu: Analog Camera Invert — None/X/Y/Both).
    s32 invert_x = 0, invert_y = 0;
    recomp_get_analog_inverted_axes(&invert_x, &invert_y);
    if (invert_x) input_x = -input_x;
    if (invert_y) input_y = -input_y;

    // Per-axis sensitivity, 0-100 with 50 = the tuned default rate, so the
    // effective rate is base * (sens / 50): 100 is ~2x speed, 0 stops the axis.
    s32 sens_x = 50, sens_y = 50;
    recomp_get_analog_cam_sensitivity(&sens_x, &sens_y);
    f32 yaw_rate = (f32)ACAM_YAW_RATE_PER_S * ((f32)sens_x * (1.0f / 50.0f));
    f32 pitch_rate = (f32)ACAM_PITCH_RATE_PER_S * ((f32)sens_y * (1.0f / 50.0f));

    // Quadratic response curve (input * |input|): fine control near center,
    // full rate at full tilt.
    if (input_x != 0.0f) {
        f32 curved = input_x * (input_x < 0.0f ? -input_x : input_x);
        s32 delta = (s32)(-curved * yaw_rate * dt);
        g_analog_cam_yaw = (s16)(g_analog_cam_yaw + delta);
    }
    if (input_y != 0.0f) {
        f32 curved = input_y * (input_y < 0.0f ? -input_y : input_y);
        s32 pitch = (s32)g_analog_cam_pitch + (s32)(curved * pitch_rate * dt);
        if (pitch > ACAM_PITCH_MAX) pitch = ACAM_PITCH_MAX;
        if (pitch < ACAM_PITCH_MIN) pitch = ACAM_PITCH_MIN;
        g_analog_cam_pitch = (s16)pitch;
    }

    // v11: the host counter-rotation hack is OFF (pass 0). Movement now
    // follows the rotated view natively: the patched func_801CE3F0 makes
    // the resolver derive its basis from the yaw-rotated camera, so the
    // game maps the left stick itself. (Restore g_analog_cam_yaw here to
    // roll back to the v9/v10 dual-basis behavior.)
    recomp_set_analog_cam_yaw(0);
}

// Rebuild a Camera's eye from the held offset and the accumulated analog
// yaw/pitch, anchored to the live look_at. Rotating a fixed-length offset makes
// the eye−look_at distance invariant by construction (no radius drift), and
// leaving look_at untouched keeps the aim on the character while we own the
// orbit angle — which is also the basis the movement resolver reads.
static void acam_rotate_in_place(Camera* cam) {
    // Live eye offset. At entry `cam` is an unrotated copy of the live game
    // camera, so this is the game's current framing: `r` is its intended
    // distance and `ly` its intended height, both of which stay the game's to
    // set (see the g_acam_dir_* comment).
    f32 lx = cam->position.x - cam->look_at.x;
    f32 ly = cam->position.y - cam->look_at.y;
    f32 lz = cam->position.z - cam->look_at.z;
    f32 r = __builtin_sqrtf(lx * lx + lz * lz);

    // Snapshot only the azimuth, on the first frame of an engagement. Yaw is ~0
    // at that instant, so the engage transition is seamless.
    if (!g_acam_captured) {
        // Degenerate: camera directly overhead, so there is no horizontal
        // direction to capture. Defer to the next frame rather than snapshot a
        // meaningless one — passing the camera through untouched meanwhile.
        if (r < 1.0f) {
            return;
        }
        g_acam_dir_x = lx / r;
        g_acam_dir_z = lz / r;
        g_acam_captured = 1;
    }

    // Yaw: rotate the captured unit direction about the vertical axis, then
    // scale by the LIVE radius. This is an ABSOLUTE world azimuth we own — it
    // does not read the live camera's (drifting) azimuth, so the follow-cam
    // cannot pull the view back; but the game keeps control of how far out and
    // how high the camera sits, so a dolly-out or a new area's framing is
    // picked up automatically. Recomputed from g_acam_dir_* each frame rather
    // than accumulated, so the direction cannot drift off unit length.
    f32 s = acam_sin(g_analog_cam_yaw);
    f32 c = acam_cos(g_analog_cam_yaw);
    f32 dx = (g_acam_dir_x * c - g_acam_dir_z * s) * r;
    f32 dz = (g_acam_dir_x * s + g_acam_dir_z * c) * r;
    f32 dy = ly;

    // Pitch: swing the eye vertically in the (horizontal-distance, height)
    // plane about the look_at, radius-preserving, with the absolute elevation
    // clamp [−10°, +55°]. Positive pitch raises the eye.
    if (g_analog_cam_pitch != 0) {
        f32 d = __builtin_sqrtf(dx * dx + dz * dz);
        if (d > 1.0f) {
            f32 ps = acam_sin(g_analog_cam_pitch);
            f32 pc = acam_cos(g_analog_cam_pitch);
            f32 nd = d * pc - dy * ps;
            f32 ny = d * ps + dy * pc;

            f32 radius = __builtin_sqrtf(nd * nd + ny * ny);
            if (nd < radius * ACAM_ELEV_MAX_C) {
                // Past +55° (incl. behind-vertical): pin to the +55° ray.
                if (ny > 0.0f) {
                    nd = radius * ACAM_ELEV_MAX_C;
                    ny = radius * ACAM_ELEV_MAX_S;
                } else if (ny < -radius * ACAM_ELEV_MIN_S) {
                    nd = radius * ACAM_ELEV_MIN_C;
                    ny = -radius * ACAM_ELEV_MIN_S;
                }
            } else if (ny < -nd * (ACAM_ELEV_MIN_S / ACAM_ELEV_MIN_C)) {
                // Below −10°: pin to the −10° ray.
                nd = radius * ACAM_ELEV_MIN_C;
                ny = -radius * ACAM_ELEV_MIN_S;
            }

            f32 scale = nd / d;
            dx *= scale;
            dz *= scale;
            dy = ny;
        }
    }

    // Reconstruct the eye from the live look_at + our held/rotated offset. The
    // look_at (game-updated to track the player) is left untouched, so the aim
    // stays on the character while we own the orbit angle.
    cam->position.x = cam->look_at.x + dx;
    cam->position.y = cam->look_at.y + dy;
    cam->position.z = cam->look_at.z + dz;

    // Level the horizon while the analog camera drives the view: the view
    // builder's up-vector is (sin r, cos r, 0) from the roll halfword at
    // Camera+0x18, which is only correct for the view azimuth the game set
    // it for — after a yaw it tilts the horizon (seen on the v12 footage).
    cam->unknown_18 = 0;
}

Camera* apply_analog_camera(Camera* cam) {
    if (cam == NULL) {
        return cam;
    }
    if (!recomp_get_analog_cam_enabled()) {
        return cam;
    }
    if (!acam_in_gameplay()) {
        return cam;
    }
    if (!acam_is_gameplay_camera(cam)) {
        return cam;
    }
    // Before the first engagement (nothing captured), zero displacement means
    // the game drives — pass it straight through. Once engaged we ALWAYS
    // reconstruct, even at yaw/pitch == 0, so the held pose stays put instead of
    // flashing back to the live (drifted) game camera for a frame.
    if (g_analog_cam_yaw == 0 && g_analog_cam_pitch == 0 && !g_acam_captured) {
        return cam;
    }

    // Rotate a private copy so game camera state is never mutated.
    static Camera rotated;
    memcpy(&rotated, cam, sizeof(Camera));

    acam_rotate_in_place(&rotated);

    return &rotated;
}

// ============================================================================
// v11 — native movement basis. RECOMP_PATCH of the movement-resolver wrapper
// func_801CE3F0_58A300 (0xE0 bytes, faithfully reproduced from the recompiled
// disasm). While the analog camera is active, the Camera pointer the resolver
// core (func_801CE4D0) reads through *(*(*(task+0x64)+0x18)+0x2C) is swapped
// to a yaw-rotated copy for the duration of the call, so the movement basis
// matches the rendered view exactly. This replaces the host left-stick
// counter-rotation (the proven root cause of the settle-inversion jank:
// v9/v10 rotated a private render copy while the resolver kept reading the
// unrotated camera).
// ============================================================================

// Rotated Camera image fed to the resolver during the pointer swap.
static Camera acam_basis_cam;

// Faithful reproduction of func_801CE3F0_58A300 (see docs/re-notes).
// task+0x74..0x7C = output basis slot (zeroed up front, filled by the core);
// *(task+0x64) = camera controller object (flag byte +0x5E bit 1 = resolver
// disabled); *0x8020CA2C = processed-stick record (+0xC magnitude,
// +0x10/+0x14 planar); *0x801FC624+0xAB = camera-cut blend counter.
RECOMP_PATCH s32 func_801CE3F0_58A300(u8* task, f32 speed) {
    *(f32*)(task + 0x74) = 0.0f;
    *(f32*)(task + 0x78) = 0.0f;
    *(f32*)(task + 0x7C) = 0.0f;

    u32 obj = *(u32*)(task + 0x64);
    if (*(u8*)(obj + 0x5E) & 2) {
        return 0x400;
    }

    u8* rec = (u8*)*ACAM_MOVE_PTR;
    f32 mag = *(f32*)(rec + 0xC);

    if (mag == 0.0f) {
        u32 ctl = *(u32*)(task + 0x5C);
        if (!(*(u16*)(ctl + 0x0) & 1)) {
            *(u8*)(*(u32*)0x801FC624 + 0xAB) = 0;
            *(u8*)(*(u32*)(task + 0x5C) + 0x78) = 0;
            return 0x400;
        }
    }

    // Planar stick components; x is negated on both paths (branch delay
    // slot in the original), and both are normalized when magnitude != 0.
    f32 x = -*(f32*)(rec + 0x10);
    f32 z = *(f32*)(rec + 0x14);
    if (mag != 0.0f) {
        x /= mag;
        z /= mag;
    }

    if (speed == 0.0f) {
        return 0x400;
    }

    // @recomp Basis interposition: point the camera node's Camera pointer at
    // a yaw-rotated copy across the resolver-core call, then restore it.
    // Single-threaded game logic — nothing else can observe the swap window.
    u32 node = 0;
    u32 old_cam = 0;
    s32 swapped = 0;
    if (recomp_get_analog_cam_enabled() &&
        (g_analog_cam_yaw != 0 || g_analog_cam_pitch != 0 || g_acam_captured) &&
        acam_in_gameplay()) {
        node = *(u32*)(obj + 0x18);
        if (node >= 0x80000000u && node < 0x80800000u) {
            old_cam = *(u32*)(node + 0x2C);
            Camera* real = (Camera*)(old_cam & 0x8FFFFFFE);
            if ((u32)real >= 0x80000000u && (u32)real < 0x80800000u &&
                acam_is_gameplay_camera(real)) {
                memcpy(&acam_basis_cam, real, sizeof(Camera));
                acam_rotate_in_place(&acam_basis_cam);
                *(u32*)(node + 0x2C) = (u32)&acam_basis_cam;
                swapped = 1;
            }
        }
    }

    s32 ret = func_801CE4D0_58A3E0(task, (f32*)(task + 0x74), x, z, speed);

    if (swapped) {
        *(u32*)(node + 0x2C) = old_cam;
    }

    return ret;
}

// ============================================================================
// Skybox yaw. The sky is NOT 3D geometry — it is a 640x240 panorama blitted as
// screen-space texture rectangles (ROM segment labelled "# Skybox"; the draw
// path is what patches/background.c already patches). func_801F8670_5B4580
// computes its horizontal scroll as
//
//     u = 640 - 640 * (binang yaw of eye-look_at) / 1024
//
// i.e. one full panorama per full turn, reading the LIVE camera through
// *(*(*(player+0x64)+0x18)+0x2C) — the same chain the movement resolver uses.
//
// That chain saw NEITHER of our hooks: apply_analog_camera rotates a private
// copy and never writes the node's +0x2C word, and the func_801CE3F0 swap is
// restored a few instructions later, long before the skybox task runs. So the
// sky kept following the game's own follow-cam azimuth — which the v14
// absolute-override deliberately stopped driving — and stayed nearly put while
// the world swung. Same fix as the movement basis: swap the node's Camera
// pointer to a rotated copy across the call, so the scroll is computed from the
// view we are actually rendering. Correct by construction: the game's own
// formula is fed the camera it would have had.
//
// (The gentle sky drift while merely WALKING is native behaviour, not this bug
// — that is the follow-cam azimuth lazily tracking the player.)
// ============================================================================

// Rotated Camera image fed to the skybox scroll computation during the swap.
static Camera acam_sky_cam;

void func_801F8670_5B4580(void* node, s32 width, s32 height);

// Faithful reproduction of func_801F8644_5B4554 (0x2C bytes): it forwards its
// SECOND argument to func_801F8670 with the panorama dimensions 640 x 136. The
// first argument is stored to the stack and never used.
// Point the camera node's Camera pointer at a rotated copy for the duration of
// a skybox scroll computation. Returns 1 if the swap was made, in which case
// *out_node / *out_old must be handed back to acam_sky_unswap afterwards.
//
// Resolves exactly the chain func_801F8670 reads. When no player exists that
// function instead falls back to the camera at *(0x800C7ADC) and there is no
// node pointer to swap — the range checks below leave the swap off in that
// case, which is correct (it is not gameplay anyway).
static s32 acam_sky_swap(u32* out_node, u32* out_old) {
    if (!recomp_get_analog_cam_enabled() ||
        (g_analog_cam_yaw == 0 && g_analog_cam_pitch == 0 && !g_acam_captured) ||
        !acam_in_gameplay()) {
        return 0;
    }

    u32 player = *(u32*)0x801FC604;
    if (player < 0x80000000u || player >= 0x80800000u) {
        return 0;
    }
    u32 obj = *(u32*)(player + 0x64);
    if (obj < 0x80000000u || obj >= 0x80800000u) {
        return 0;
    }
    u32 node = *(u32*)(obj + 0x18);
    if (node < 0x80000000u || node >= 0x80800000u) {
        return 0;
    }

    u32 old_cam = *(u32*)(node + 0x2C);
    Camera* real = (Camera*)(old_cam & 0x8FFFFFFE);
    if ((u32)real < 0x80000000u || (u32)real >= 0x80800000u ||
        !acam_is_gameplay_camera(real)) {
        return 0;
    }

    memcpy(&acam_sky_cam, real, sizeof(Camera));
    acam_rotate_in_place(&acam_sky_cam);
    *(u32*)(node + 0x2C) = (u32)&acam_sky_cam;

    *out_node = node;
    *out_old = old_cam;
    return 1;
}

static void acam_sky_unswap(u32 node, u32 old_cam) {
    *(u32*)(node + 0x2C) = old_cam;
}

RECOMP_PATCH void func_801F8644_5B4554(void* task, void* node_arg) {
    (void)task;

    u32 node = 0, old_cam = 0;
    s32 swapped = acam_sky_swap(&node, &old_cam);

    func_801F8670_5B4580(node_arg, 0x280, 0x88);

    if (swapped) {
        acam_sky_unswap(node, old_cam);
    }
}

// The SECOND caller of the skybox scroll computation — a scripted/animated sky
// variant in a different overlay, which func_801F8644's patch does not cover.
// Confirmed a real alternate path rather than a symbol-resolution artifact:
// it passes DATA-DRIVEN panorama dimensions read from the node (+0x10/+0x12),
// where the base-exe forwarder hardcodes 640 x 136. A duplicated symbol would
// have identical code.
//
// Faithful reproduction of func_802242DC_68DCBC (0xA4 bytes). Dispatches on the
// mode byte at task+0x5D: 0 = advance a two-axis scroll-offset tween and publish
// it into the node, 1 = run the scroll computation, anything else = return.
RECOMP_PATCH void func_802242DC_68DCBC(u8* task, void* arg1) {
    (void)arg1;

    u8 mode = *(u8*)(task + 0x5D);

    if (mode == 0) {
        u8* p = task + 0x5C;
        s16 count = *(s16*)(p + 0x2);
        f32 scroll_u;

        if (count > 0) {
            f32 u_cur  = *(f32*)(p + 0x8);
            f32 u_step = *(f32*)(p + 0xC);
            f32 v_cur  = *(f32*)(p + 0x14);
            f32 v_step = *(f32*)(p + 0x18);

            *(s16*)(p + 0x2) = count - 1;
            *(f32*)(p + 0x8) = u_cur + u_step;
            *(f32*)(p + 0x14) = v_cur + v_step;
        }
        scroll_u = *(f32*)(p + 0x8);

        u32 node = *(u32*)(p + 0x20);
        *(f32*)(node + 0x14) = scroll_u;
        node = *(u32*)(p + 0x20);
        *(f32*)(node + 0x18) = *(f32*)(p + 0x14);
    } else if (mode == 1) {
        u8* sky_node = (u8*)*(u32*)(task + 0x7C);

        u32 node = 0, old_cam = 0;
        s32 swapped = acam_sky_swap(&node, &old_cam);

        func_801F8670_5B4580(sky_node, *(u16*)(sky_node + 0x10),
                             *(u16*)(sky_node + 0x12));

        if (swapped) {
            acam_sky_unswap(node, old_cam);
        }
    }
}

// ============================================================================
// Positional audio panning. func_8000FE1C_10A1C derives the LISTENER heading
// from the camera's eye->look_at azimuth and pans with
//
//     sin( (sourceYaw - cameraYaw) & 0x3FF )
//
// Unlike the skybox and the movement resolver, it does NOT resolve the camera
// through the node chain — the Camera is passed as an ARGUMENT ($a1), untouched,
// down the chain func_8000F420 -> func_8000F6E8 -> func_8000FE1C. So a
// node+0x2C pointer swap cannot reach it; the argument has to be substituted.
//
// Left unfixed this is wrong EVERYWHERE, always, by exactly the accumulated
// analog yaw: orbit 90 degrees and a sound source visibly on your left plays
// from your right; at 180 degrees the whole soundstage is mirrored. It is
// inaudible on mono/TV speakers, which is why it survived visual testing.
//
// Hooked at func_8000F6E8 rather than at func_8000FE1C itself: F6E8 is a thin
// 0x48-byte forwarder (vs 0x360 for FE1C) and still covers the overwhelming
// majority of entries — the ~130 overlay sites that call func_8000F420, plus
// F6E8's own 8 direct and 5 indirect callers.
//
// KNOWN GAP, deliberate: four sites call func_8000FE1C directly and bypass this
// patch — 0x801F2A58 and 0x801F2A7C (RecompiledFuncs/funcs_34.c), 0x8000FE04
// (funcs_11.c, inside the audio module itself), and 0x80210074 (funcs_87.c).
// Closing those means faithfully reproducing all 0x360 bytes of FE1C. Revisit
// only if a specific sound is audibly mispanned while the rest are correct.
// ============================================================================

// Rotated Camera image handed to the audio panner during the substitution.
static Camera acam_audio_cam;

// Args are forwarded verbatim as raw words: a2/a3 and the two stack slots carry
// float BITS through integer registers in the original (an mtc1/mfc1 round trip
// that is a no-op), so typing them as u32 reproduces the o32 layout exactly and
// avoids the compiler re-classifying them into FP registers.
void func_8000FE1C_10A1C(u32 sound_id, void* cam, u32 x, u32 z, u32 a, u32 b);

// Faithful reproduction of func_8000F6E8_102E8 (0x48 bytes): masks the sound id
// to 16 bits and forwards every other argument unchanged.
RECOMP_PATCH void func_8000F6E8_102E8(u32 sound_id, void* cam, u32 x, u32 z,
                                      u32 a, u32 b) {
    void* use_cam = cam;

    if (cam != NULL && recomp_get_analog_cam_enabled() &&
        (g_analog_cam_yaw != 0 || g_analog_cam_pitch != 0 || g_acam_captured) &&
        acam_in_gameplay()) {
        Camera* real = (Camera*)(((u32)cam) & 0x8FFFFFFE);
        if ((u32)real >= 0x80000000u && (u32)real < 0x80800000u &&
            acam_is_gameplay_camera(real)) {
            memcpy(&acam_audio_cam, real, sizeof(Camera));
            acam_rotate_in_place(&acam_audio_cam);
            use_cam = &acam_audio_cam;
        }
    }

    func_8000FE1C_10A1C(sound_id & 0xFFFF, use_cam, x, z, a, b);
}
