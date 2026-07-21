# Goemon64Recomp-Android — working context for Claude Code

Private Android port of klorfmorf's Goemon64Recomp (N64 static recompilation,
Zelda64Recomp/N64Recomp ecosystem). GitHub: `ogdanimal/Goemon64Recomp-Android`
(PRIVATE). Default branch `main`, work branch `dev`. Remotes: `origin`=ogdanimal,
`upstream`=klorfmorf.

## Environment
- Repo lives in WSL at `~/projects/Goemon64Recomp`; work as a normal (non-root) user
  (no sudo/root needed — the repo was relocated out of `/root` on 2026-07-17).
- `gh` is authed as `ogdanimal`; git commit identity is
  `ogdanimal <306057786+ogdanimal@users.noreply.github.com>` (GitHub noreply, set
  2026-07-20; the old `the.ogdanimal@gmail.com` remains on earlier commits and is
  fine to stay public).
- **`gh` defaults to `upstream` (klorfmorf) unless told otherwise** — with no
  default set it prefers the remote named `upstream` over `origin`, so bare
  `gh run list` silently returns klorfmorf's public workflow runs (they even have
  a `dev` branch, so the output looks plausible — it just isn't ours). Fixed in
  this clone via `gh repo set-default ogdanimal/Goemon64Recomp-Android`, which
  writes `remote.origin.gh-resolved = base` into `.git/config`. **That is local
  to the clone and will NOT survive a fresh clone — re-run it after cloning**,
  or pass `--repo ogdanimal/Goemon64Recomp-Android` explicitly.
- ROM `mnsg.z64` (32 MiB, gitignored) sits at the repo root — the decompressed US ROM
  the recompiler consumes. Never commit it.
- **Back up on-device data BEFORE installing anything that changes storage paths,
  and before any uninstall / `pm clear`.** Verify the backup by CHECKSUM, not by
  `adb pull`'s success message. Latest: `%USERPROFILE%\goemon-backups\2026-07-19\data`
  (93 files, 43 MB — saves, configs, ROM, assets). Saves live in
  `<dataDir>/saves/mnsg.us.bin` plus `.bak` and `.manual.bak`.
  NOTE: `mnsg.us.z64` in the data dir is written by the native runtime when it
  registers the ROM — it is NOT a stale duplicate of `mnsg.z64`, do not "clean it up".

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
- **Autosave is FUNCTIONALLY COMPLETE (2026-07-19).** All five steps done and
  device-verified: manual trigger, sharpened differential test, `.manual.bak`
  rollback point, save-data-settled check, and the 2-minute timer. Pushed on
  `dev`. See the Autosave section below and `docs/autosave.md`.
  - **DECIDED 2026-07-19: autosave stays Off by default. This is settled, not
    an open call** — do not re-raise it without the user. Every technical gate
    for On is met and verified; it stays Off because the feature reimplements
    Goemon's save routine, and the differential test proved the payload matches
    on the states tested, not on every state that exists. Off means anyone
    exposed to that opted in.
  - **"Defaults Off" means the WHOLE feature is off, not just the timer.**
    There is one setting, `autosave_mode` (`src/game/config.cpp:266`), and the
    early return at the top of `update_autosave` (`patches/autosave.c:605`) sits
    *above* the combo handling — so with the default in place the manual
    `L + R + Z` combo does nothing either, the settle tracking does not
    accumulate, and the Saved indicator is never seen. On a fresh install the
    feature is entirely inert until the player enables it. Earlier notes said
    "the timer defaults to Off", which wrongly implies manual saving works out
    of the box. The default applies only when the JSON key is **absent**;
    existing users keep whatever they chose.
  - **On-screen "Saved" indicator — DONE, device-verified 2026-07-19** on both
    the timed and manual paths (`src/ui/ui_saved_indicator.cpp`). The settings
    description now also states the 2-minute interval. The old claim here that
    it "needs the RT64 extended-GBI path" was **wrong**: that path has no text
    primitive, while a `recompui` context is just an RmlUi *document* and
    `set_captures_input(false)` is exactly the toast/modal switch. See
    `docs/autosave.md` § "The Saved indicator" — the mistake is the reusable
    part. Note the two hazards recorded there: the tick MUST stay above
    `draw_hook`'s launcher-return check so an expiring toast updates
    `is_any_context_shown()` before it is read (NOT a deadlock concern —
    `ui_state_mutex` is a `std::recursive_mutex` that `draw_hook` itself re-enters;
    the earlier "non-recursive → self-deadlock" note was wrong, corrected
    2026-07-20), and the guest thread may only touch an atomic.
  - Interval configurability: **DECIDED 2026-07-20 — stays a `#define`, NOT
    configurable.** The user confirmed the fixed 2-minute interval is fine; do not
    re-raise. (Settings text hardcodes "every 2 minutes", which now matches intent.)
- **Analog camera consumer fixes — DONE and DEPLOYED 2026-07-19** (`8dc748d`
  code, `80e9ace` docs, pushed to `dev`). Four fixes, all device-verified by the
  user: area-transition reset via the map id at `0x800C7AB2`; capture the
  AZIMUTH only so the game keeps control of framing distance/height (it starts
  very close on area entry and eases out over ~2s); skybox scroll in BOTH
  callers; and positional-audio pan. See the Analog camera section below and
  **`docs/re-notes/README.md`**, which is now the entry point for all RE notes.
  - **The bug CLASS to keep in mind:** the analog camera never mutates the real
    Camera — it hands a rotated private copy to specific consumers. Any consumer
    NOT hooked silently reads the UNROTATED camera and disagrees with the
    rendered view. The handled/unhandled list is in `docs/re-notes/README.md`.
  - **Storage-side rotation (one hook instead of per-consumer hooks) was
    re-litigated on 2026-07-19 and REFUTED. Do not re-open** — no hook point is
    both downstream of all producers and upstream of all consumers, and
    `func_80012878` seeds camera tweens FROM the live camera at 22 sites across
    13 overlays. Evidence in `goemon_default_cam_writer.md` §6(3).
- **SD-card game data — DONE and DEPLOYED 2026-07-19** (`d14af04`). Users can
  keep the ROM, assets, configs and saves on a removable card, chosen ONCE at
  first run. `DataPaths.java` is the single source of truth for the data dir —
  do not go back to computing it per-activity, which is how the two callers
  drifted before. Existing installs are grandfathered to internal and never
  prompted. Device-verified both ways: a fresh install landed everything on
  `/storage/<uuid>/` with internal left empty, and an existing install saw no
  prompt with save checksums unchanged.
  - **DECIDED: card pulled MID-GAME is not handled. Do not "fix" it.** Reasoning
    is in `DataPaths`' class comment. Availability is checked at launch only.
  - No migration exists by design; the first-run choice is final short of a
    reinstall. Adding migration later means copy-verify-then-delete, never move.
- **Vulkan diagnostics — DEPLOYED 2026-07-19** (`4a6552f`, via plume `65783a0`
  and rt64 `8c73b3f`). Startup now logs the selected physical device (vendor,
  device API, driver version) and the loader's Vulkan version vs what we
  request. Aimed at making bug reports from other people's hardware diagnosable
  without a round trip. **All three submodule levels are pushed to their forks**
  — verify with `git ls-remote` before trusting a pointer bump, since a bump
  committed while the submodule commit stays local is unbuildable by CI.
- **Controller input rework + analog-camera zoom — DONE, DEVICE-VERIFIED, and
  SHIPPED IN v1.0.0 2026-07-20.** Committed and pushed on `main`/`dev`.
  Authored fresh from `docs/input/n64-goemon-input-assignment.csv` (the design
  doc — keep it in sync with the code).
  - **New default controller map** (`default_n64_controller_mappings`,
    `src/game/input.cpp`), Xbox face layout: A=SOUTH, B=EAST, **X=WEST→C-Up**,
    **Y=NORTH→C-Left** (X/Y swapped from the CSV's first pass per user), RB=C-Down,
    LB=**N64 L**, LT=Z, RT=R, Start, Select=menu. Right stick → C-buttons
    (suppressed→camera in analog mode). **Physical D-pad → C-buttons; N64 D-pad
    left UNBOUND** (menu nav reads the physical D-pad directly via SDL in
    `ui_state.cpp`, so menus are unaffected).
  - **`bindings_per_input` raised 2→3** (`include/recomp_input.h`) so each
    C-button holds face + right-stick + D-pad. In analog mode the stick binding
    self-suppresses, leaving face + D-pad — this is how "D-pad covers C while the
    stick drives the camera" works with no mode-swap logic.
  - **Analog-camera ZOOM** (`patches/camera.c`): hold **R (RT) + right-stick Y**
    = uniform dolly zoom (`g_analog_cam_zoom`, applied in `acam_rotate_in_place`;
    tunables `ACAM_ZOOM_RATE_PER_S` 0.9, `ACAM_ZOOM_MIN/MAX` 0.45/2.5). Stick up
    = in; resets with recenter/area-change/disengage; added to all four "engaged"
    predicates so zoom-only still reconstructs. Distance was already read live, so
    zoom is just a scale — azimuth-neutral, safe for the movement/skybox/audio
    consumers.
  - **Native N64 R suppressed while analog cam On** (`get_n64_input`,
    `controls.cpp`) so holding RT to zoom does not hijack the C-buttons into the
    game's own camera control (and native R+C zoom cannot compound). The zoom
    modifier reads the PHYSICAL trigger via the new
    `recomp_get_camera_zoom_held()` (plumbed: `recomp_api.cpp` / `main.cpp`
    REGISTER / `syms.ld` 0x8F000084 / `input.h` / `recomp_input.h` /
    `input.cpp`), so it is unaffected by the mask. R is untouched in Standard mode.
  - **Save combo moved to `L + R + Z`** and its R part now reads the physical
    trigger too, so it survives the R-mask in both modes. See the Autosave
    section.
  - **GOTCHA (will recur every default-binding change):** new defaults only apply
    to inputs ABSENT from the saved `controls.json`; existing installs keep their
    file. To pick up new defaults: in-game **Controls → Reset to Defaults**, or
    delete `controls.json` **and** `controls.json.bak` on device (both, or the
    `.bak` reloads) then relaunch. Fresh installs get the new defaults for free.
    On device the config lives at
    `/storage/<uuid>/Android/data/com.goemon64.recomp/files/data/`.
- **NOW: v1.0.0 is PUBLIC, RELEASED, and DEVICE-VERIFIED on the release build
  (2026-07-20).** The repo is public, the first signed release is live, and the
  user smoke-tested the release-signed APK on the RP5 (launches + loads a restored
  save). Monitor the GitHub **issue tracker** for
  device-specific bug reports (Vulkan/driver issues on non-Adreno GPUs are the
  likely class) and triage; otherwise back to general bug-fixing. Test via the CI
  debug APK, or cut a new signed release by pushing a `v*` tag (see release setup
  below).
- **Code-review remediation — DONE, build+CI-verified, PUSHED on `dev`
  (2026-07-20; tip `a27223a`).** A full-codebase critique (Java launcher, host
  `src/`, `patches/`, CI) fixed the high tier (H1–H7) and the long tail (L1–L14).
  Findings record with per-item verified-vs-review status and outcomes:
  **`docs/code-review-2026-07-20.md`** (cite it, don't re-derive). Things that
  change how the codebase works:
  - **New CI guard** (`patches/Makefile`): fails the patch link if any *undefined*
    `recomp_*` symbol exists — catches a host export referenced in patch C but
    missing from `syms.ld`, which `--unresolved-symbols=ignore-all` otherwise
    links as address 0 → wrong host fn at runtime. Game/libultra symbols stay
    undefined by design and are NOT flagged. Needs `llvm-nm` (both workflows
    `apt-get install llvm`). Soft spot, not hardened: if `llvm-nm` ever goes
    missing the guard passes vacuously (`command -v` preflight is the fix if wanted).
  - **Launcher `version_string` is now build-wired** (H6): gradle `vName`
    (v-stripped) → `-DGOEMON_VERSION_STRING` → CMake define → `main.cpp` macro,
    `1.0.0-dev` fallback for local host builds. No more hardcoded `0.2.0-dev`.
  - **Controller-map race + handle leak + UAF fixed** (`src/game/input.cpp`): all
    four SDL-thread writers take `cur_controllers_mutex`; disconnect closes the
    handle AND purges the freed pointer from `cur_controllers` before the close.
  - **`onDestroy` no longer kills the app on an accessibility font/display-size
    change** (`fontScale|density` added to `configChanges`; `halt(0)` guarded by
    `isFinishing()`).
  - **attack_move resets its frozen direction on area transition** (map-id
    `0x800C7AB2` guard mirroring camera.c) — first post-transition attack no
    longer lunges the previous area's heading.
  - **Analog-cam pointer swap hardened** (L9): both swap sites OR the original
    word's flag bits into the replacement (`| (old_cam & ~0x8FFFFFFEu)`) so the
    node word is bit-faithful DURING the swap window, not just after the restore.
    An initial strike of this finding was WRONG and was reopened — the lesson is
    in memory `review-findings-need-verification`.
  - **`ui_state_mutex` is `std::recursive_mutex`** (draw_hook re-enters it at
    `:738`); the old "non-recursive → self-deadlock" note (here and in comments)
    was FALSE and corrected here, in the source comments, and in the autosave §
    note. The tick runs before the launcher check for a visibility-ordering
    reason, not deadlock.
  - Device-verified: latest debug build `install -r`'d onto the RP5 (2026-07-20),
    saves intact (backed up + checksummed first — see [[device-install-method]]).
  - **BACKLOG (M-tier, NOT started):** launcher main-thread I/O (32 MB ROM copy +
    45 MB asset extract on the UI thread — M2, biggest), recents-relaunch
    storage-guard bypass (M4), non-atomic cross-thread settings ints (M6),
    config-save reported success on disk-full (M7), `ui_prompt` callback-under-lock
    (M9), widescreen stale-static (M10), autosave engine-guard off-by-one probe
    (M8). All in the doc. L12 (annotated commented-out reference blocks) is
    intentional/not-actionable; L8 (versionCode) is done — see release section.
- **Attack While Moving — DONE, device-verified, COMMITTED on `dev` (`9bae463`),
  but NOT pushed and deployment HELD (2026-07-20, user's explicit call).** A
  novelty setting `attack_while_moving_mode` (default **Off**) that lets the
  player keep moving during a ground attack instead of rooting in place. Wired
  end-to-end like the "Swap While Moving" toggle (config+JSON, `ui_config.cpp`,
  `recomp_api` export `recomp_get_attack_while_moving_enabled` @ `0x8F000088`,
  settings row + D-pad nav in `general.rml`). Feature lives in
  `patches/attack_move.{c,h}`; inert when Off.
  - **How it works:** the game zeroes the player's velocity for the WHOLE attack
    (device-probed across 900+ frames — the attack action handlers never run the
    movement integrator), so movement is *re-injected* by writing a per-frame
    displacement onto the authoritative head position node `*(0x801FC60C)+0x8/10`.
    Direction is **sampled from the game's own world motion during real
    locomotion and frozen for the swing** — reconstructing it from the camera
    basis MOONWALKED (the azimuth disagreed with the character's facing, and no
    sign flip fixed it; it was also a feedback loop when sampled during the
    attack). An ease envelope ramps it in/out; a short post-attack HOLD cancels
    the game's recovery re-anchor (the residual "slide back to origin" the user
    saw). Movement is NOT collision-checked during the swing (inferred, not
    verified — do not claim wall-clipping as fact; the wall-clip line was cut
    from the UI copy for exactly that reason).
  - **Covered action states (device-identified via a temporary action_id
    logger):** melee/jump family `0x58`–`0x5E`, ryo throw `0x7C`, bombs `0x90`.
    **Hookshot/extension `0x70`–`0x72` deliberately EXCLUDED** — it anchors the
    pipe to a world point; sliding during it risks desyncing the grapple (ladder
    hazard class). This extends the char-swap RE `action_id` map.
  - **Gotcha reused:** the RML change needs the device's `.assets_version` stamp
    deleted or the old UI stays extracted. Node-chain writes crash the renderer
    (a pointer field at `+0x8`) — only the proven vec3f head node is safe to poke.
- **PARKED — do not start without the user's say-so:**
  - **World→screen projector fix** (`func_8001CB40` / `func_8001CC38_1D838`) —
    parked 2026-07-19. Mechanism is confirmed wrong (screen-space overlays are
    positioned from the unrotated camera) but nothing player-visible has been
    observed routing through it, and the fix means reproducing ~65 instructions
    of dense float math where a transcription slip would misplace everything
    silently. UNPARK TRIGGER: a 2D effect seen sitting beside rather than on
    whatever produced it, after orbiting. Full detail + the `+0x1A` fov-vs-roll
    resolution in `docs/re-notes/README.md`.
  - Quest64-Recomp Android port (separately scoped; different rt64 lineage makes the
    graphics work non-trivial).

## Public repo + release process (DONE 2026-07-20)
- **Repo is PUBLIC.** The `validate-external` fork-PR gate turned out UNNEEDED —
  neither workflow triggers on `pull_request`, so fork PRs can't run CI or reach
  the ROM secret. Before flipping, the whole repo + history + submodules + APK were
  scrubbed of PII (local usernames, device labels, the maintainer's name) and
  secret-scanned (gitleaks, 0 real findings). Builds also strip absolute source
  paths via `-ffile-prefix-map` (top of `CMakeLists.txt`), so no compiled binary
  (local debug or CI release) embeds the builder's home dir / username. GitHub hardening enabled: **secret scanning, push protection,
  Dependabot** (Dependabot has opened PRs for `assets/scss` build-tooling npm
  deps — build-time only, not shipped in the APK).
- **Releases are tag-triggered + signed** via `.github/workflows/android-release.yml`:
  push a `v*` tag → recompile → signed `assembleRelease` → publishes a GitHub
  Release with the APK. `versionName` = the tag (`v1.0.0` → 1.0.0). `versionCode`
  is **tag-derived semver** `major*10000+minor*100+patch` (changed 2026-07-20,
  was `git rev-list --count` which drifts on a history rewrite — this repo has
  scrubbed history once; `build.gradle` reads `-PvName`/`-PvCode`). The release
  workflow now also REJECTS a tag that isn't `vMAJOR.MINOR.PATCH[-suffix]` or has
  minor/patch ≥ 100; `-rc` tags intentionally share their final release's code.
  v1.0.0 shipped code 661 (old scheme); any release ≥ v1.0.1 → ≥ 10001, so the
  switch is monotonic. **v1.0.0 is released.** A `v1.0.1-rc1` dry-run tag would
  prove the derivation end-to-end before the next real cut.
  Repo secrets: `RELEASE_KEYSTORE_BASE64`, `RELEASE_STORE_PASSWORD`,
  `RELEASE_KEY_ALIAS` (`goemon-upload`), `RELEASE_KEY_PASSWORD` (+ the existing
  `G64RS_REPO_WITH_PAT` ROM secret).
- **Signing keystore = ONE-WAY DOOR.** PKCS12, backed up + checksum-verified at
  `%USERPROFILE%\goemon-backups\keystore-2026-07-20\goemon-release.jks`. Signing
  cert **SHA-256 `47323a0b…`** — every legitimate update MUST be signed with this
  same key/cert or it won't install over an existing install. `.gitignore` ignores
  `keystore.properties`/`*.jks`/`*.keystore`.
- To cut an update: bump nothing manually — just `git tag vX.Y.Z && git push origin
  vX.Y.Z` on a clean tip. Do a `v*-rc*` tag first to dry-run the pipeline if unsure.

## Autosave (COMPLETE — all 5 steps device-verified, pushed on dev)
Resume prompt for a fresh session: `docs/re-notes/RESUME-autosave.md`.
Feature doc: `docs/autosave.md`. RE: `docs/re-notes/goemon_save_re.md`.
Evidence corpus (cite it, don't re-derive): `docs/re-notes/fixtures/`.

Commits on `dev`: `70d3e4d` feature, `9522fe8` docs+RE corrections,
`49fedf8` `.bak` hazard reframing, `71e56a5` fixtures.

WHAT WORKS: manual trigger `L + R + Z` (was `L + R + D-Pad Up`; changed 2026-07-20
when the reworked controller map bound the physical D-pad to the C-buttons,
making N64 D-Up unproducible — Z is the third harmless input alongside L and R).
Edge-triggered. The single `autosave_mode` setting defaults **Off**, which
disables this combo too — see the note above.
- **Analog-camera zoom interaction (RESOLVED 2026-07-20):** analog-camera zoom
  uses R (right trigger) as its modifier, so `get_n64_input` masks N64 R out of
  the button word while Analog Camera mode is On. The `L+R+Z` combo therefore
  reads its **R part from the physical trigger** (`recomp_get_camera_zoom_held`),
  so it works identically in both modes; L and Z stay in the N64 button word. See
  docs/autosave.md.
Commits through Goemon's own save data/slot format by reimplementing
`func_80214D58_5D0228` (overlay code, so not callable). Writes the slot the
player loaded — which is the game's *native* semantic, since an in-game save
offers no slot choice either. Verified on device: `status 0`, correct slot
including a non-zero one, and the differential test PASSED (payload
byte-identical to the game's own routine apart from CRC + a play-time byte).
`SAVE_SLOT_COUNT = 3` confirmed.

TWO REMAINING (in `RESUME-autosave.md`):
1. Sharpen the differential test — the passing one compared near-identical
   state ("equal when nothing changed"). Change hearts `+0x6C` / ryo `+0x74` /
   stage `+0x200` first, then autosave + NPC-save and confirm both moved
   identically. Compare `cmp -i 256` — a whole-file `cmp` is a FALSE FAILURE.
2. Rollback mechanism — **DONE, VERIFIED ON DEVICE 2026-07-19.** `.manual.bak`
   matched the manual save and survived two autosaves 4.6s apart (the original
   loss scenario) while `.bak` became an autosave. The timer's gating
   precondition is MET. Design changed
   late: **observe guest pak writes host-side**, no game function patched. The
   earlier "notify via `RECOMP_PATCH func_8000B718_C318`" plan is **SUPERSEDED —
   do not implement it** (no `RECOMP_HOOK` in this toolchain, so `RECOMP_PATCH`
   replaces the whole function). Two generic hooks in the `lib/N64ModernRuntime`
   submodule + all policy in `src/game/save_rollback.cpp`. (An earlier note here
   said the submodule commit was **not yet pushed to `fork`** so CI could not
   build the pointer bump — **STALE, corrected 2026-07-19**: the recorded pointer
   `b6f6253` matches the `ogdanimal/N64ModernRuntime` `goemon-android` tip, and
   CI is green.) Killed two old
   worries: the active-disarm rule (suspends never reach the pak) and the
   fixtures byte-identity precondition (now vacuous). Pak diagnostics
   reachability: **RESOLVED, UNREACHABLE, HIGH confidence.**
3. Save-data-settled check — **DONE, VERIFIED ON DEVICE 2026-07-19.** Watches 4
   ranges across the live block `0x8015C5D8` AND gamedata (gamedata alone is
   stale for HP/ryo/lives — only the marshal writes `gd+0x64`). GOTCHA: the
   first passing run proved nothing, since an inert check would look identical;
   two provoked positive controls were needed. See `docs/autosave.md` § "The
   settled check" before changing the watched ranges.
4. **The timer — DONE, VERIFIED ON DEVICE 2026-07-19.** 2-min interval; saves
   only when safe + settled + something actually changed (walking changes
   nothing watched, so idle/walking produces no writes). Full cycle observed.
   Ships **Off** by default along with the rest of the feature — DECIDED
   2026-07-19, settled, see the note near the top of this file.

RULE THIS FEATURE EARNED THE HARD WAY (3 separate occasions): for anything that
refuses or declines, build the diagnostic that proves it is REACHING ITS
DECISION POINT. Silence looked like success in the differential test (header
residue), the settled check (would have been inert), and the timer (looked
dead). A passing test with no such diagnostic proves nothing.

GOTCHAS THAT COST TIME (all documented, repeated here because they bite):
- `0x800C7A9E` is a 3-phase init/run/exit counter, gameplay == **1** not 0. The
  old note had it inverted and it refused every save.
- The gate's `andi 0x1000` reads a BUTTON word; `0x1000` is `CONT_START`. It is
  a trigger, not a safety guard. Do not add it as a guard.
- Step 10 (`func_80023698_24298`) writes `0x100` bytes and leaks `0xF0` bytes of
  uninitialised stack into the file. Two saves of identical state ALWAYS differ
  there. Nothing reads past `0x0F`; it is not a bug.
- `.bak` is NOT a recovery copy once autosave is enabled — `files.cpp:39-45`
  rotates on every flush, and every autosave is a flush. True of the *current*
  build, not just a future timer. `.manual.bak` is the deliberate rollback point
  and is device-verified (survives autosaves); `.bak` is still autosave data.
- `sub 3` in an autosave refusal is NOT dialogue — standing in an inn reads
  `sub 3` too, with `busy 0`. Dialogue additionally sets `busy` non-zero. The
  gate is faithful: the game's own pause menu also refuses in that inn spot, so
  autosave does not fire in inns.
- `make -C patches` needs `CC=clang LD=ld.lld`; an inherited `CC=cc` defeats the
  Makefile's `CC ?= clang`. `~/goemon-build-all.sh` passes both.

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
ZOOM — DONE, device-verified, and SHIPPED IN v1.0.0 2026-07-20 (hold R/RT +
right-stick Y; see the input-rework bullet in "Current focus"). FEEL/POLISH:
**DECIDED 2026-07-20 — good as-is, do not tune proactively.** The user confirmed
the rates, clamps, pitch-sign, and the Y-invert↔zoom coupling all feel right;
revisit only if players actually complain. Build = one call:
`wsl -d Ubuntu bash ~/goemon-build-all.sh`.
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
only on zone-trigger cuts (set-active-camera choke point func_8001C3E0;
& 0x8FFFFFFE = uncached→cached ptr mask; dead-code player-follow snapper
func_801F8FD0 exists but is never called). So consumer-side hooks are the
CORRECT final architecture, not a workaround.
- CORRECTION 2026-07-19: 0x801684A0/0x801684F0 were listed here as "active
  globals". They are NOT camera state — they are func_80016C44's per-object
  animation/skeleton scratch, reassigned for every drawn object every frame
  (patches/anime.c:50,108 already decompiles them that way). Useless as a
  camera-cut signal. For area transitions use the map id at 0x800C7AB2.
- RE-LITIGATED AND RE-CLOSED 2026-07-19: rotating the real camera in place
  (one hook instead of per-consumer hooks) was investigated properly and is
  REFUTED. (a) No hook point is simultaneously downstream of all camera
  producers and upstream of all consumers — they interleave inside the same
  object walk. (b) func_80012878 seeds camera tweens FROM the live camera at
  22 sites across 13 overlays, so a rotated struct would make every scripted
  camera move ease from the player's orbit pose. DO NOT re-open; details and
  the consumer/handled list are in docs/re-notes/README.md.
- READ docs/re-notes/README.md BEFORE trusting any goemon_*.md: it carries
  standing methodology warnings (jal-only scans produce false negatives;
  validate a scan pattern against a known positive before trusting a zero;
  identify struct fields by how their trig is consumed) plus the corrections
  those warnings triggered.
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
