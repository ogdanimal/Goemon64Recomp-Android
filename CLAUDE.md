# Goemon64Recomp-Android — working context for Claude Code

Private Android port of klorfmorf's Goemon64Recomp (N64 static recompilation,
Zelda64Recomp/N64Recomp ecosystem). GitHub: `ogdanimal/Goemon64Recomp-Android`
(PRIVATE). Default branch `main`, work branch `dev`. Remotes: `origin`=ogdanimal,
`upstream`=klorfmorf.

## Environment
- Repo lives in WSL at `/home/user/projects/Goemon64Recomp`; work as user `user`
  (no sudo/root needed — the repo was relocated out of `/root` on 2026-07-17).
- `gh` is authed as `ogdanimal`; git identity is `ogdanimal <the.ogdanimal@gmail.com>`.
- ROM `mnsg.z64` (32 MiB, gitignored) sits at the repo root — the decompressed US ROM
  the recompiler consumes. Never commit it.

## Build reality (important)
- A clean `--recurse-submodules` clone canNOT build without the ROM: `RecompiledFuncs/`
  and `RecompiledPatches/` are generated (not committed) by running `N64Recomp mnsg.toml`
  and `RSPRecomp aspMain.toml` against `mnsg.z64`. The ROM is a BUILD-time input; its
  data never ships in the APK (APK `assets/` = only the mod's fonts/RmlUi UI).
- Submodule fork chain (all on branch `goemon-android`, remote `fork`=ogdanimal):
  `lib/rt64`→ogdanimal/rt64, `lib/N64ModernRuntime`→ogdanimal/N64ModernRuntime,
  and nested `lib/rt64/src/contrib/plume`→ogdanimal/plume (carries the Android
  `setRenderWindow`/ANativeWindow surface fix). Repo is self-contained for recursive clone.

## CI — DONE & GREEN
`.github/workflows/android.yml` builds the arm64-v8a **debug** APK on GitHub-hosted
runners on every push to `dev`/`main`. It pulls the ROM from the PRIVATE companion repo
`ogdanimal/Goemon64RecompSecrets` via the secret `G64RS_REPO_WITH_PAT` (an authenticated
clone URL), runs the host recompile + host `file_to_c` + patches codegen, then
`gradle assembleDebug`. NDK 27.1.12297006, CMake 3.22.1. Actions pinned to node24 runtimes.
- Grab the latest test APK: `gh run download <run-id> --repo ogdanimal/Goemon64Recomp-Android`
  (or the run's Artifacts). Debug-signed → sideloadable; asks for the user's ROM on first launch.

## Current focus & parked work
- **NOW: bug-fixing** on the Android port (test via the CI debug APK).
- **PARKED — do not start without the user's say-so:**
  - Flip repo private→public (deliberate call; when doing it, add the `validate-external`
    fork-PR authorize-gate so fork PRs can't reach the ROM secret).
  - Release setup: signed, tag-triggered (`v*`) GitHub Releases — do around/after going
    public; requires generating + BACKING UP an upload keystore first (one-way door).
    `.gitignore` already ignores `keystore.properties`/`*.jks`/`*.keystore`.
  - Quest64-Recomp Android port (separately scoped; different rt64 lineage makes the
    graphics work non-trivial).

## Analog camera (v14 — WORKING, committed on dev)
Full resume prompt for a fresh session: `docs/re-notes/RESUME.md`.
v14 = the two root-cause fixes that made it actually work (2026-07-18,
on-device confirmed: orbit is clean, no drift while walking):
(1) SINE CONVENTION BUG — `func_80003E10_4A10` (math_sin) returns a true
unit sine but its FULL TURN = 0x400 (1024), NOT 0x10000. camera.c had
assumed 0x10000 and added 0x4000 for cosine → that offset is 16 whole
0x400 periods, so sin==cos aliased to one table entry (matrix
non-orthonormal). The "rotation" silently RESCALED the eye radius each
frame (rOut collapsing to 0) → the camera swung INTO the player then
flipped. Proof: widescreen.c masks its arg `& 0x3FF`; back-solved s=c=0.733
= sin(1402 mod 1024). Fix: `acam_sin(a)=func_80003E10_4A10((a>>6)&0x3FF)`,
cos offset `0x100`. (This DISPROVES the old note that these trig helpers
take 0x10000-unit angles.)
(2) ABSOLUTE-OVERRIDE (Zelda64Recomp `update_analog_camera_params` model)
— rotating the LIVE camera inherited Goemon's follow-cam, which swings its
own azimuth behind movement, so the view drifted back while walking. Now:
on the first engaged frame, snapshot the eye→look_at offset
(`g_acam_captured` + `g_acam_off_{x,y,z}`), then each frame rotate THAT held
offset by accumulated yaw/pitch and re-anchor to the LIVE look_at (which
still tracks the player). Overrides the game azimuth → no drift; look_at is
now the pitch/orbit pivot (was the player node). Capture resets on leaving
gameplay so re-engaging never jumps. The `func_801CE3F0` basis swap now
also triggers on `g_acam_captured` so walking stays view-relative.
TRADEOFF (by design): once engaged, the game's auto-follow is overridden
for the rest of the area (no auto-return-to-behind-you). Optional slow
drift-back is a possible follow-up.
Zelda ref confirmed helpful: their `update_analog_cam` accumulates spherical
yaw/pitch, `update_analog_camera_params` re-syncs from the live camera when
idle (`Math_Atan2S(eye−at)`) and rebuilds eye via `OLib_AddVecGeoToVec3f`
(= at + geo(r,pitch,yaw)); `camera_transform_tagging.c` is frame-interp
tagging (orthogonal, not used).
DONE since: menu settings added (per-axis invert via the pre-existing
`CameraInvertMode` plumbing; Camera Sensitivity X/Y, 0-100 with 50 = the tuned
base rate; R3 = drop the override so the game's follow-cam resumes), the
dependent rows grey out + focus-disable while Analog Camera is Off
(`data-attrif-disabled`, NOT `data-attr-` — the latter sets the attribute even
when false), and the dense [acamR]/[acamU]/[acamB]/[acamH] diagnostics are
STRIPPED. Committed on dev as 83daa6a.
NEXT (optional polish): feel tuning of the base rates now that sensitivity is
adjustable; pitch-sign default (stick up = eye rises) still unconfirmed;
C-button UX. Build = one call: `wsl -d Ubuntu bash ~/goemon-build-all.sh`.
GOTCHA: after any `assets/` change, delete the device's
`files/data/.assets_version` stamp or the app keeps the OLD extracted UI.

v13 fixes the v12 footage artifacts: (1) horizon roll — the view builder
func_80017D8C reads a ROLL binang halfword from Camera+0x18 (unknown_18)
and builds guLookAtHilite's up = (sin r, cos r, 0); a game-set roll is only
valid for the original azimuth, so the rotated copy now zeroes +0x18;
(2) extreme pitch — absolute radius-preserving eye-elevation clamp
[−10°, +55°] on top of the game's base angle (near-vertical views made the
fixed up-vector degenerate). [acamP] now prints pitch too.
ARCHITECTURE CLOSED (goemon_default_cam_writer.md): there is NO per-frame
default camera writer — default Camera = static BSS 0x8020CBF0, poses set
only on zone-trigger cuts (set-active-camera choke point func_8001C3E0,
active globals 0x801684A0/0x801684F0; & 0x8FFFFFFE = uncached→cached ptr
mask; dead-code player-follow snapper func_801F8FD0 exists but is never
called). So consumer-side hooks (v9 render rotation + CE3F0/CE4D0 basis
swap) are the CORRECT final architecture, not a workaround.
v12 = v11 + feel/pitch: TIME-BASED rates via recomp_time_us (yaw 0x5800
binang/s, pitch 0x2C00/s, quadratic response curve, 50ms dt clamp — fixes
the call-rate-dependent touchiness of the old per-call 0x200), plus pitch:
eye swings vertically about the player pivot in the (dist,height) plane,
look_at fixed, clamp +0x2C00/−0x1400, eye-azimuth (and so the movement
basis azimuth) untouched; basis swap covers pitch too. Pitch sign: stick
up = positive input_y = eye rises (flip the sign on `curved` in
update_analog_camera if the user wants inverted).
Basis-verify agent CONFIRMED: basis is movement's ONLY camera input,
rotation is 1:1 (goemon_basis_verify.md).
Right-stick analog camera via render-side rotation. v10 = v9 + player-pos
orbit pivot ([acamP] CONFIRMED live: pos tracks the player incl. height).
v11 = v10 + native movement basis: RECOMP_PATCH of func_801CE3F0_58A300
(faithful C reproduction in patches/camera.c) swaps the camera node's
Camera pointer (*(node+0x2C), node=*(objA+0x18)) to a yaw-rotated copy
around the func_801CE4D0 call, so the resolver's basis matches the rendered
view; the host left-stick counter-rotation is OFF (recomp_set_analog_cam_yaw(0)).
(The old "slow camera spiral / drift while walking" watch-item is RESOLVED by
the v14 absolute-override above — the follow-cam no longer bleeds in.)
Files (all committed in 83daa6a): patches/camera.{c,h}, patches/anime.c,
patches/{input.h,main.c,syms.ld}, src/game/{input,controls,recomp_api,config}.cpp,
src/main/main.cpp, src/ui/ui_config.cpp, include/{recomp_input.h,goemon_config.h},
assets/config_menu/general.rml, assets/recomp.rcss, docs/re-notes/.

### RE findings (2026-07-17 static-RE sweep, critic-verified; reports in
### the session scratchpad goemon_*.md — copy them somewhere durable!)
- PLAYER POSITION (static proof, triple convergence): pos node =
  *(u32*)0x801FC60C (spawn-time cache of player->0x18; player object =
  *(u32*)0x801FC604, both written once in func_801CB5D0_5874E0). World pos
  f32 x/y/z at node+0x8/0xC/0x10; integrator func_801CC4C0_5883D0 adds
  velocity onto exactly these offsets. Heading binang = *(u16*)0x8020C904.
  Engine-wide rule: object world transform is behind *(obj+0x18)
  (pos +0x8/C/10, rot u16 +0x14/16/18).
- INVERSION ROOT CAUSE (proven): movement resolver func_801CE4D0_58A3E0
  (sole pose→basis converter; single caller chain func_801CD310→
  func_801CE3F0) builds its basis from the LIVE render Camera struct:
  eye−look_at read via *(*(*(player+0x64)+0x18)+0x2C) & 0x8FFFFFFE. v9/v10
  rotate a private copy, so the resolver sees the unrotated camera. Complete
  fix = rotate the eye−at delta by the analog yaw inside func_801CE4D0
  (built at 0x801CE514-44 into sp+0x4C/50/54, before normalize @0x801CE558)
  and DROP the host counter-rotation hack. Do NOT rotate the stick record.
- *(u32*)0x8020CA2C is NOT a movement object: it is re-pointed every tick by
  func_801CC4C0 at a static 0x18-byte processed-STICK record
  (0x800C7DB0 + 24*idx): +0xC magnitude, +0x10/+0x14 planar components.
  Still valid as the in-gameplay gate.
- func_0800037C_72E5DC is a scripted EARTHQUAKE-CUTSCENE camera (overlay
  rombase 0x72E260, one sibling func_08000B54_72EDB4, no per-area copies).
  Camera tween module 0x80012304..0x80012940: func_80012900 commits a
  0x60-byte image from *(handle+0x6C) onto the Camera via an INTEGER copy
  loop (swc1 scans are blind to this pattern). No shared base-exe eye/at
  helper exists; scripted overlays inline their camera writes (~6 large +
  ~10 partial across ~12 overlays).
- Default-area follow-cam writer still UNFOUND (last gap). Leads: template
  0x8020CBF0 members (callback 0x801D23C8 is a no-op stub), callers of
  func_80012900/func_80012878, vram gap 0x800126E0-0x80012810, integer-copy
  loops aliasing *(0x801FC628)+0x2C. 0x801FC628 = camera view node
  (*(objA+0x18), objA=*(0x801FC624)); its +0x2C holds the render Camera ptr.
- *(0x8015CD60) is a per-area-repurposed static param block (3 readers in
  the whole binary; movement resolver does NOT read it) — wrong lever.

NEXT: (1) confirm [acamP] pos tracks the player on device → pivot fix
validated; (2) patch func_801CE4D0 (reimplement in patches/, rotating the
eye−at delta) and remove the host counter-rotation → kills the inversion
quirk; (3) optional: find the default cam writer for a true native orbit.
