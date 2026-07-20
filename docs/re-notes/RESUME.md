# Resume prompt — analog camera work (v14, 2026-07-18)

## v14 status (top of stack — read this first)
**DONE.** The camera works on device (clean orbit, no drift while walking),
diagnostics are stripped, and it is COMMITTED + PUSHED on `dev` as `ad0e51b`
(26 files; `e67f31b` follows with .gitignore hygiene). Two root-cause fixes on
top of v13 are what made it work:
1. **Sine convention** — `func_80003E10_4A10` (math_sin) has FULL TURN = 0x400
   (1024), not 0x10000. camera.c assumed 0x10000 and used `+0x4000` for cosine
   (= 16 whole periods → sin==cos aliased → non-orthonormal matrix that
   rescaled the eye radius each frame → "swing into the player then flip").
   Fix: `acam_sin(a)=func_80003E10_4A10((a>>6)&0x3FF)`, cosine offset `0x100`.
   Proof: widescreen.c masks its arg `& 0x3FF`. (Disproves any note claiming
   these trig helpers take 0x10000-unit angles.)
2. **Absolute-override** (Zelda `update_analog_camera_params` model) — was
   rotating the LIVE follow-cam, so the game's azimuth drift bled the view back
   while walking. Now snapshots the eye→look_at offset on the first engaged
   frame (`g_acam_captured`/`g_acam_off_*`), rotates that held offset by the
   accumulated yaw/pitch, and re-anchors to the LIVE look_at each frame (which
   tracks the player). look_at is now the pitch/orbit pivot. Capture resets on
   leaving gameplay. `func_801CE3F0` basis swap also triggers on
   `g_acam_captured`. Tradeoff (by design): once engaged, game auto-follow is
   overridden for the rest of the area.
ALSO SHIPPED in `ad0e51b`: menu settings — per-axis invert (the
`CameraInvertMode` host plumbing already existed; only the RML widget + patch
consumption were missing), Camera Sensitivity X/Y (0-100, 50 = the tuned base
rate), and R3 = drop the override so the game's follow-cam resumes. The three
dependent rows grey out and focus-disable while Analog Camera is Off, via
`data-attrif-disabled` (NOT `data-attr-`, which sets the attribute even when
false and would disable them permanently). The dense
[acamR]/[acamU]/[acamB]/[acamH] diagnostics are GONE, along with the then-dead
`acam_get_pivot`/`ACAM_PLAYER_POS_NODE` (the override pivots on look_at now).

NEXT: nothing outstanding. Feel/rates, pitch-sign (stick up = eye rises), and
the Y-invert↔zoom coupling were all confirmed good and DECIDED settled
2026-07-20 (see CLAUDE.md § "Analog camera"); zoom shipped in v1.0.0. Do not
tune proactively.

Build in ONE call: `wsl -d Ubuntu bash ~/goemon-build-all.sh` (avoids the
PowerShell→wsl.exe quote mangling — see [[wsl-bash-tool-gotchas]]). After ANY
`assets/` change, delete the device's `files/data/.assets_version` stamp or the
app keeps serving the OLD extracted UI and your edit looks like it never built.

---
# (v13 prompt below — historical context)

Copy-paste the block below to resume in a fresh session.

---

Resume the analog (right-stick) camera work on Goemon64Recomp-Android.
Repo: `~/projects/Goemon64Recomp` (WSL "Ubuntu", branch dev). Read
CLAUDE.md first — its "Analog camera" section is authoritative; full
critic-verified RE reports live in `docs/re-notes/goemon_*.md` (8 files).
All camera work is UNCOMMITTED on dev. Device: Retroid Pocket 5, adb serial
<device-serial> (Windows-side adb only; WSL adb can't see it).

STATE: **v13 deployed** (built, installed, briefly test-driven — diagnostics
healthy, awaiting the user's final verdict). The architecture is DONE and
verified; remaining work is feel-tuning and polish only.

ARCHITECTURE (closed — do not re-open the writer hunt):
- The game has NO per-frame camera writer. Default Camera = static BSS
  0x8020CBF0; poses are CUT-BASED (zone triggers; set-active-camera choke
  point func_8001C3E0; active globals 0x801684A0/0x801684F0). The
  `& 0x8FFFFFFE` seen in readers is just uncached→cached pointer masking.
  Consumer-side hooks are therefore the *correct* design, not a workaround:
  1. Render: `apply_analog_camera()` in patches/anime.c case 0x20000000
     rotates a private Camera copy before func_80017D8C_1898C builds the view.
  2. Movement: RECOMP_PATCH of wrapper func_801CE3F0_58A300 (faithful C repro
     in patches/camera.c) swaps `*(node+0x2C)` (node = `*(objA+0x18)`,
     objA = `*(task+0x64)`) to the same rotated copy around the
     func_801CE4D0_58A3E0 call — the resolver derives its movement basis from
     eye−look_at of the LIVE Camera, so this makes movement match the view
     natively. Host left-stick counter-rotation is OFF
     (`recomp_set_analog_cam_yaw(0)`), verified 1:1 by agent
     (goemon_basis_verify.md).

HARD-WON FACTS (do not re-derive):
- Player world pos: node = `*(u32*)0x801FC60C`, f32 x/y/z at +0x8/0xC/0x10
  (spawn-time cache of player→0x18; player obj = `*0x801FC604`). Heading
  binang = `*(u16*)0x8020C904`. Engine rule: object transform behind
  *(obj+0x18): pos +0x8/C/10, rot u16 +0x14/16/18. Live-confirmed ([acamP]).
- `*(u32*)0x8020CA2C` = processed-STICK record (static, 0x800C7DB0+24·idx;
  +0xC magnitude, +0x10/+0x14 planar), NOT a movement object. Still the
  in-gameplay gate (valid ptr only during gameplay).
- Camera+0x18 (u16) = ROLL binang; view-builder up = (sin r, cos r, 0). The
  rotated copy must zero it (v13 does) or game-set roll tilts the horizon
  after a yaw. Near-vertical views degenerate this up-vector → keep the
  absolute eye-elevation clamp.
- Never rotate the bit-exact UI camera eye=(0,0,100) at=(0,0,0).
- update_analog_camera may run multiple times per rendered frame → all rates
  are TIME-BASED via recomp_time_us (u32 µs); publish state, never
  consume-and-null.
- Right-stick raw-joystick fallback in src/game/input.cpp::get_right_analog
  with the <10%-deflection trigger — KEEP IT.
- func_0800037C_72E5DC = earthquake-cutscene camera (dead end);
  `*(0x8015CD60)` = per-area param block (wrong lever); func_801F8FD0 is a
  dead-code player-follow snapper (never called — historical curiosity).

V13 FEEL CONSTANTS (patches/camera.c — the tuning surface):
- ACAM_YAW_RATE_PER_S 0x5800 (~124°/s), ACAM_PITCH_RATE_PER_S 0x2C00, both
  with quadratic response curve and 50ms dt clamp.
- Accumulated pitch clamp +0x2000/−0x1000; ABSOLUTE eye-elevation clamp
  [−10°, +55°] (radius-preserving, in acam_rotate_in_place).
- Pitch sign: stick up = eye rises (flip `curved` sign in
  update_analog_camera to invert).

TASK (pick up from the user's feedback): tune feel constants / pitch sign
per their test verdict; candidate polish items: yaw bleed-while-walking
(game's own follow heading ctl+0xA4 makes the real camera swing behind
movement over time), roll-zero snap when engaging the camera in rolled
areas, C-button suppression UX. If all good: squash the work into a clean
commit on dev (patches/camera.{c,h}, patches/anime.c, patches/input.h,
patches/syms.ld, src/game/{input,controls,recomp_api}.cpp,
include/recomp_input.h, assets/config_menu/general.rml + docs/re-notes/ +
CLAUDE.md).

BUILD/TEST LOOP (patches + host):

    source ~/goemon-buildenv.sh && cd ~/projects/Goemon64Recomp \
      && make -C patches CC=clang LD=ld.lld && ./N64Recomp patches.toml \
      && ./build-host-tools/file_to_c patches/patches.bin mm_patches_bin \
           RecompiledPatches/patches_bin.c RecompiledPatches/patches_bin.h \
      && cd android && ./gradlew assembleDebug --no-daemon

APK: android/app/build/outputs/apk/debug/app-debug.apk (copy to Windows
before adb install — Git Bash mangles UNC/device paths; use PowerShell).
Windows: adb -s <device-serial> install -r <apk>; force-stop; logcat -c; launch.
Logcat tag Goemon64-stdio; diags: [acamP] yaw/pitch/player-pos rubber
stamp, [acamB] basis-swap engagement, [acamH] input mapping.
Testing needs the user (human) — batch diagnostics; screen recordings land
in /sdcard/Movies/ (pull via PowerShell adb, frame-dump with WSL ffmpeg).
