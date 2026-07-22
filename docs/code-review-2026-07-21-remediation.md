# Code-review remediation session — 2026-07-21

Complete record of the changes made this session, working the
`docs/code-review-2026-07-20.md` (pass 1, M-tier) and
`docs/code-review-2026-07-20-pass2.md` (pass 2 tail) backlogs by priority.

**Branch:** all work committed and pushed to `dev`. `main` is untouched at the
`v1.0.2` release tag; these ship in the next cut (`v1.0.3`).

**Verification:** every batch was built green via `~/goemon-build-all.sh` (patches
codegen → `N64Recomp patches.toml` → `file_to_c` → `gradle assembleDebug`) and
passed the `patches/Makefile` undefined-`recomp_*` link guard. No on-device
testing yet (held at the maintainer's request — device is on release-signed
v1.0.2-rc1; a fresh checksum-verified save backup was taken to local storage,
`goemon-backups/2026-07-21-preinstall-debug/`, outside the repo).

---

## Commits (oldest → newest on `dev`)

| Commit | Batch | Findings |
|--------|-------|----------|
| `7330bd3` | crash / deadlock / data-loss | S3 (host), M7, M9, N10, N16 |
| `ca21489` | M-tier | M6, M10 |
| `1fbbb30` | B-tier build/tooling | B1, B2, B3, B4 |
| `c487b13` | S3 completion (submodule) | S3 (N64ModernRuntime gitlink bump) |
| `1db4d63` | S2 | S2 |
| `dab5be7` | N-tier | N6, N7, N8, N9, N11, N12, N13, N14, N15 |
| `5f04fec` | C/D/R tail | C4, C5, C6, D1, D2, D3, D4, D5, R1, R2 |

Submodule: `lib/N64ModernRuntime` bumped `b6f6253 → 920d493` (fork
`ogdanimal/N64ModernRuntime` branch `goemon-android`, `git ls-remote`-verified
pushed before the gitlink bump so CI can build it). rt64 / plume / N64MR-other
tips unchanged.

---

## 1. Crash / deadlock / silent-data-loss (`7330bd3`)

| # | File | Change |
|---|------|--------|
| **M7** | `src/game/config.cpp` | `save_json_with_backups` now `flush()`es and checks `good()` after the `<<` write, still inside the ofstream scope, and returns `false` (skipping `finalize`) on a bad stream. A failed write (disk full) no longer promotes a truncated temp file over the existing good config. |
| **S3 (host)** | `src/game/config.cpp` | `save_config()` and `load_config()` use the non-throwing `std::error_code` `create_directories` overload (`save_config` early-returns on failure; `load_config` falls back to defaults). A vanished SD data dir no longer SIGABRTs on close-settings. |
| **M9** | `src/ui/ui_prompt.cpp` | Prompt cancel callbacks no longer run under the non-recursive `prompt_state.mutex`. `show_prompt` is `[[nodiscard]]` and **returns** the previous cancel action instead of calling it under the lock; the four openers (`open_choice_prompt`/`open_three_choice_prompt`/`open_info_prompt`/`open_notification`) wrap their body in a `{}` scope and invoke it after the lock releases; `close_prompt` moves the action into a local and invokes it after the scope. Behavior-preserving; kills the self-deadlock when a cancel action re-enters the prompt API. |
| **N10** | `patches/autosave.c` | New `autosave_log_slot()` validates `G_SAVE_CTX_PTR` is in RDRAM and returns `-1` otherwise; the three status-log sites use it instead of an unguarded deref, so a failed save's log no longer prints a garbage slot. |
| **N16** | `src/game/recomp_api.cpp` | `recomp_get_target_framerate` clamps `frame_divisor` to `>= 1` before `60 / divisor` (latent SIGFPE). |

## 2. M-tier (`ca21489`)

| # | File | Change |
|---|------|--------|
| **M6** | `src/ui/ui_config.cpp` | Every `ControlOptionsContext` (4 ints, 8 enums, 2 sensitivity ints) and `CheatsContext` (3 enums) field is now `std::atomic<T>`, mirroring `SoundOptionsContext` — the game/render/input threads read these each frame while the UI thread writes them. Getters/setters compile unchanged (atomic's implicit `operator T()` / `operator=(T)`, seq_cst — correct here). RML binding: the 6 int sliders use the existing `bind_atomic`; the 11 enum options use a new `bind_atomic_option` (atomic twin of `bind_option`, identical DirtyVariable behavior); general bindings fetch their model handle before binding, like sound. |
| **M10** | `patches/widescreen.c` | The karakusa ripple handler (`func_80213AC8_672A78`) now reads/writes its own `g_rippling_karakusa_previous_aspect_ratio` instead of the hikimaku's static, so a runtime aspect change no longer makes the second-initialized background keep a stale scale. |

## 3. B-tier build / tooling (`1fbbb30`)

| # | File | Change |
|---|------|--------|
| **B1** | `android/app/build.gradle`, `README.md`, `BUILDING.md` | Dropped the no-op `-DGOEMON_ANDROID=ON` CMake arg (consumed by nothing; CMake gates on the NDK's built-in `ANDROID`) and corrected the two docs that taught it. Kills the CMake unused-variable warning. |
| **B2** | `patches/Makefile` | `CC := clang` (was `CC ?= clang`, which never fired because make predefines `CC=cc`, so bare `make -C patches` used host cc). A command-line `CC=…` still overrides, so the build script / CI are unchanged. |
| **B3** | `CMakeLists.txt` | The `compile_commands.json` convenience symlink is now guarded `AND NOT WIN32` (needs Developer Mode there) and non-fatal via `RESULT_VARIABLE`. |
| **B4** | `README.md` | Build section now states a supported ROM is required at build time (host codegen generates `RecompiledFuncs/`/`RecompiledPatches/`) and links `BUILDING.md`. |

## 4. S2 — same-process MainActivity recreation (`1db4d63`)

| # | File | Change |
|---|------|--------|
| **S2** | `AndroidManifest.xml`, `MainActivity.java` | (1) `configChanges` gains `fontWeightAdjustment\|mcc\|mnc\|colorMode\|grammaticalGender` (all valid at compileSdk 34), so those config classes no longer recreate the activity. (2) A process-lifetime latch `sNativeInitedThisProcess` (set after the first `nativeInit`, never reset) mirrors the native one-way latch; if `onCreate` runs again in the same process (any recreation vector that slips past configChanges — memory reclaim, "don't keep activities", an unlisted config change) it bounces to `RestartActivity` (own process → kills this pid → clean cold start, or LauncherActivity if the SD volume is gone) instead of a doomed second `nativeInit` (the "syms.ld" modal / black screen). `singleTask` routes icon-relaunch to `onNewIntent`, not `onCreate`, so the fast-resume path never trips it. |

## 5. S3 completion — submodule (`c487b13`)

| # | File | Change |
|---|------|--------|
| **S3 (submodule)** | `lib/N64ModernRuntime` @ `920d493` | `read_save_file` (`pi.cpp`, hit at ROM registration — on failure falls through to the open, which zero-fills the save buffer) and `initialize_mods` (`recomp.cpp`) use the non-throwing `create_directories` overload too. Completes the host-side S3 fix. Gitlink bumped `b6f6253 → 920d493`. |

## 6. N-tier (`dab5be7`)

| # | File | Change |
|---|------|--------|
| **N6** | `src/ui/ui_state.cpp` | Applied the `lock + null-check` pattern to the `ui_state` entry points that raced teardown: `activate_mouse` (event thread) and `queue_image_from_bytes_{file,rgba32}` (guest thread) had **no lock at all**; plus `set_cont_active`, and the missing null-check under the existing lock in `show_context`/`hide_context`/`load_document`/`create_empty_document`. |
| **N7** | `src/game/input.cpp` | Sensor accel/gyro handlers use `find()` not `operator[]` (no more zombie `controller_states` entry after a REMOVED); ADDED closes an existing different handle (and purges `cur_controllers`) before overwriting `.controller`. |
| **N8** | `src/ui/ui_state.cpp` | `config_was_open = true` after opening the config menu, so a second same-frame menu-toggle can't re-trip `show_context`'s duplicate guard (release error dialog). *(The cosmetic "config-over-quit-prompt" overlap was deliberately NOT guarded via `is_prompt_open()` — that takes `prompt_state.mutex` under `ui_state_mutex`, a lock-order inversion vs the prompt openers.)* |
| **N9** | `patches/camera.c` | `acam_rotate_in_place` gains an `allow_capture` flag. Only the authoritative live-camera paths (render/basis/skybox) seed the one-shot analog-cam azimuth; the audio-pan hook passes 0 (applies an existing capture, never seeds), so a non-live argument camera can no longer poison the whole engagement. |
| **N11** | `src/game/input.cpp`, `src/game/controls.cpp` | 64-bit `std::abs` at both axis sites (kills `abs(INT32_MIN)` UB on a hand-edited `controls.json`), plus a missing `>= 0` axis lower bound; removed the dead `joystick_deadzone` local. |
| **N12** | `src/game/config.cpp` | `reset_graphics_options` sets `api_option = api_default` (was surviving on value-init coincidence). |
| **N13** | `src/ui/ui_config.cpp` | Removed the dead 50 ms render-thread sleep in `make_graphics_bindings` — `new_options` is valid at init (`get_graphics_config()` reads a struct populated at startup) and re-applied by the `update_supported_options` gfx_init_callback (which `DirtyAllVariables`). Explanatory comment left. |
| **N14** | `patches/autosave.c` + new host export | Timed autosave now also gated on `!recomp_is_config_menu_open()` (new export: `recomp_api.cpp` + `main.cpp` REGISTER + `syms.ld` @ `0x8F00008C` + `misc_funcs.h`) so a 2-minute boundary can't commit while the player is in the settings menu. Manual combo isn't gated (its L/Z already read the zeroed game input word while the menu is up). |
| **N15** | `src/ui/ui_state.cpp` | `update_contexts` iterates a snapshot of `shown_contexts` so a handler that shows/hides a context during `process_updates()` can't invalidate the walk. |

## 7. C/D/R tail (`5f04fec`)

| # | File | Change |
|---|------|--------|
| **C4** | `.github/dependabot.yml` (new) | Weekly `github-actions` (`/`) + `gradle` (`/android`) version updates. `gitsubmodule` deliberately excluded (fork branches carry Android patches; upstream bumps would break the build); `npm` left to repo-level security updates. Both documented in the file. |
| **C5** | `.github/workflows/android.yml` | Debug-APK artifact `retention-days: 14` (was default 90). |
| **C6** | `.github/workflows/android.yml` | Header comment: signed release builds are produced by the separate tag-triggered `android-release.yml` (exists since v1.0.0), not "a later addition". |
| **D1** | `docs/autosave.md` | Rewrote the false "non-recursive mutex → self-deadlock" hazard as "visibility ordering" (`ui_state_mutex` is recursive; `draw_hook` re-enters it). Matches the L14 source/CLAUDE.md correction. |
| **D2** | `docs/autosave.md` | The stale ".manual.bak not yet verified on device" callout now says device-verified 2026-07-19, consistent with the rest of the file. |
| **D3** | `docs/autosave.md` | Cite `AUTOSAVE_LZ_MASK` (was nonexistent `AUTOSAVE_TEST_COMBO`). |
| **D4** | `docs/re-notes/README.md` | Index line marks `RESUME-menu-framerate.md` SUPERSEDED (investigation closed 2026-07-19). |
| **D5** | `docs/input/README.md` (new), `src/game/input.cpp` | Added a legend marking the authoritative CSV columns (A–C/G) vs the stale keyboard column, the abandoned gamepad draft (I–J), and the lower scratch tables, rather than mangling the maintainer's authored CSV; pointed the code comment at it. |
| **R1** | (deleted) | Removed `android-recomp/` and `android-ui-probe/` — self-declared throwaway probes, not `add_subdirectory`'d by any build, folded into root long ago (recoverable from git history). |
| **R2** | — | Left as a maintainer keep-or-kill decision (`.github/macos/*` is live in the CMake APPLE branch); no blind deletion. |

---

## Deferred — need a device session (no code change made)

These were analysed and intentionally left for on-device verification rather than
changed blind:

- **M8** — autosave engine-guard off-by-one. Is `D_80054ACC_556CC[11].start` a
  static VRAM constant (guard vacuous → Impact battles not excluded) or a
  loaded-indicator (guard works)? The refused-state log already prints it as
  `file11 N`; probe = enable autosave, enter an Impact battle, attempt the manual
  combo, read the log.
- **N5** — R-mask is mode-global but the analog camera is engine-scoped. Needs a
  device check of whether any non-field engine reads N64 R; scoping the mask
  couples to M8's engine-detection. Changing it blind risks regressing the
  shipped analog-cam zoom.
- **S2** — recommended device check: Developer Options → "Don't keep activities",
  confirm a clean process restart (not the syms.ld modal / black screen).
- **N13** — the removed init sleep warrants a slow-device sanity check (worst case
  if the analysis is wrong is a cosmetic, self-correcting glitch, never a crash).
- **Whole session** — none of these commits are device-verified yet.

## Not started (out of scope this session)

- **P2–P7** — plume/Vulkan, weak-GPU class. Genuinely need non-Adreno hardware to
  verify; low ROI to change blind.
- **R2** — the desktop/VS scaffolding keep-or-kill decision (above).
- The 5 open Dependabot npm PRs (dev-tooling only) remain for triage.

---

## Full findings status

Per-row `→ FIXED` / `→ DEFERRED` notes are recorded inline in the two governing
docs — cite those, don't re-derive:

- `docs/code-review-2026-07-20.md` — M6, M7, M9, M10 FIXED; M8 OPEN (device probe).
- `docs/code-review-2026-07-20-pass2.md` — S2, S3, N6–N16 (except N5), B1–B4,
  C4–C6, D1–D5, R1 FIXED; N5, R2 DEFERRED.
