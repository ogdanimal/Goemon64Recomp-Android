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
// see the g_acam_off_* comment below for why that distinction fixes drift.
//
// Settings consumed from the menu: analog cam on/off, per-axis invert, per-axis
// sensitivity, and R3 to hand the camera back to the game.

// Processed analog-stick record pointer: a valid pointer only during
// gameplay (0 on the title screen and system menus) — used as the gameplay
// gate. (RE update: this is NOT a movement/heap object — func_801CC4C0
// re-points it every tick at a static 0x18-byte stick record in the table
// at 0x800C7DB0: +0xC magnitude, +0x10/+0x14 planar components.)
#define ACAM_MOVE_PTR ((u32*)0x8020CA2C)

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
static s32 g_acam_captured = 0;
static f32 g_acam_off_x = 0.0f;
static f32 g_acam_off_y = 0.0f;
static f32 g_acam_off_z = 0.0f;

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
    // Snapshot the eye offset on the first frame of an engagement (see the
    // g_acam_off_* comment). At entry `cam` is an unrotated copy of the live
    // game camera, so this captures the game's current framing exactly — the
    // engage transition is seamless (accumulated yaw is ~0 at that instant).
    if (!g_acam_captured) {
        g_acam_off_x = cam->position.x - cam->look_at.x;
        g_acam_off_y = cam->position.y - cam->look_at.y;
        g_acam_off_z = cam->position.z - cam->look_at.z;
        g_acam_captured = 1;
    }

    // Yaw: rotate the captured horizontal offset about the vertical axis. This
    // is an ABSOLUTE world azimuth we own — it does not read the live camera's
    // (drifting) azimuth, so the follow-cam can no longer pull the view back.
    f32 s = acam_sin(g_analog_cam_yaw);
    f32 c = acam_cos(g_analog_cam_yaw);
    f32 dx = g_acam_off_x * c - g_acam_off_z * s;
    f32 dz = g_acam_off_x * s + g_acam_off_z * c;
    f32 dy = g_acam_off_y;

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
