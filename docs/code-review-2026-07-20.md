# Code review — 2026-07-20

Full-codebase critique of the Android-specific code we own (Java launcher layer,
host `src/`, `patches/`, CI). Upstream N64Recomp/RT64 internals were out of scope.

Findings came from three review passes (Java+CI, native `src/`, `patches/`).
**Verification column**: `verified` = I re-read the source and confirmed it here;
`review` = asserted by the review pass, not yet independently re-checked against
source. Fix nothing marked `review` without confirming it first.

Severity: **H** fix before next release · **M** soon · **L** long tail.

---

## H — fix before next release

> **Status 2026-07-20:** H1–H7 all fixed in this pass (uncommitted, on `dev`).
> Build-verified: full `goemon-build-all.sh` → `assembleDebug` **BUILD SUCCESSFUL**,
> `input.cpp` confirmed recompiled, `-DGOEMON_VERSION_STRING="1.0.0-dev"` observed
> reaching clang. H2's fix was hardened after review caught a use-after-free in the
> first cut (see the H2 note below).


| # | Sev | Verified | Location | Issue |
|---|-----|----------|----------|-------|
| H1 | H | verified | `src/game/input.cpp:145,159,247,259` vs `:528-538` | **Data race on `controller_states`.** `sdl_event_filter` (SDL event thread) inserts/erases with no lock while the guest thread iterates under `cur_controllers_mutex`. The mutex protects nothing — the writer never takes it. Pad sleep/wake mid-frame can invalidate the iterator and crash. Guard all four writes with `cur_controllers_mutex`. |
| H2 | H | verified | `src/game/input.cpp:155-160` | **Controller handle leak.** `CONTROLLERDEVICEREMOVED` erases the map entry but never `SDL_GameControllerClose`s the handle. Every disconnect/reconnect leaks; handhelds detach the pad on every sleep. **NOTE:** the first cut of this fix closed the handle but left the now-freed pointer in the `cur_controllers` cache vector (only rebuilt once per frame by `poll_inputs`), turning a benign leak into a use-after-free on the rumble/button/analog readers. The committed fix purges the pointer from `cur_controllers` inside the same lock, before the close — all those readers hold `cur_controllers_mutex`, so the window is fully closed. |
| H3 | H | verified | `MainActivity.java:119` + `AndroidManifest.xml:44` | **`Runtime.halt(0)` on config recreation.** `configChanges` omits `fontScale`/`density`, so an accessibility font/display-size change recreates `MainActivity` → `onDestroy` with `RESTART_NONE` → `halt(0)` kills the whole app. Add `fontScale|density` to `configChanges`; guard the halt with `isFinishing()` as defense-in-depth. |
| H4 | H | verified | `src/main/main.cpp:86` | **Inverted `SDL_Init` check.** `SDL_Init(...) > 0` — SDL returns `<0` on failure, so init failure is ignored and surfaces later as an unexplained null-window crash. Use `!= 0`. |
| H5 | H | verified | `src/main/main.cpp:69-76` | **`exit_error` fold is wrong.** `((void)fprintf(stderr, str, args), ...)` calls `fprintf` once per arg with the same format string (UB for 2+ args) and prints nothing for zero args. Works today only because every caller passes one arg. Should be a single `fprintf(stderr, str, args...)`. |
| H6 | H | verified | `src/main/main.cpp:67` | **Stale version string** `"0.2.0-dev"` ships in v1.0.0 and feeds the launcher's displayed version. Nothing ties it to the release tag. |
| H7 | H | review | `.github/workflows/android.yml`, `android-release.yml` | **CI actions tag-pinned, not SHA-pinned.** Two third-party actions (`hendrikmuhs/ccache-action`, `android-actions/setup-android`) run with the ROM PAT and (release) the signing keystore + passwords in scope. A moved tag on either can exfiltrate the signing key. Pin all five by commit SHA. Also: `cp ./Goemon64RecompSecrets/* ./` sprays any future secrets-repo file into the build tree — copy `mnsg.z64` explicitly. And release builds restore ccache written by dev-branch runs — use a release-only cache key. |

## M — soon

| # | Sev | Verified | Location | Issue |
|---|-----|----------|----------|-------|
| M1 | M | review | `LauncherActivity.java:152-189` | 32 MB ROM copy + SHA-1 verify run on the UI thread, and the hash re-runs on every cold launch → multi-second freeze / ANR on slow SD cards. Move to a background thread with progress UI. |
| M2 | M | review | `MainActivity.onCreate` | 45 MB asset extraction on the UI thread on first launch after every update. Same fix; could pre-flight in `LauncherActivity`. |
| M3 | M | review | `LauncherActivity.java:166-184` | ROM copied straight to final path, no temp-file + rename + fsync. Kill mid-copy → truncated `mnsg.z64` that "Start anyway" will boot. |
| M4 | M | review | `MainActivity` (recents relaunch) | After process death Android recreates `MainActivity` directly (no `LauncherActivity`), bypassing storage guards; on an SD install with the card missing, `dataDirOrInternal` falls back and extracts a fresh internal tree, confusing grandfathering. Guard in `MainActivity`: bounce to `LauncherActivity` when the chosen volume is absent. |
| M5 | M | review | `AndroidManifest.xml` `allowBackup` | Auto Backup restores `data_location=sd` pref but not the data; restore onto a phone with no SD slot → permanent "re-insert card" loop. Exclude the pref from backup, or offer "start over on internal". |
| M6 | M | review | `src/ui/ui_config.cpp:329-548` | `control_options_context`/`cheats_context` are plain ints read every frame cross-thread while the render thread writes them; `controls.cpp:212` reads `analog_cam_mode` even with the menu open. `SoundOptionsContext` already uses atomics — mirror it. |
| M7 | M | review | `src/game/config.cpp:245-255` | Config save never checks the stream after writing; a disk-full write is promoted to current config as success. Add `flush(); if (!good()) return false;`. |
| M8 | M | review | `patches/autosave.c:219` vs `required.c:31` | Engine-module guard may be off by one: comment says `.file_11` but under the `file_id - 1` convention `[11]` is file id 12 (the save overlay, not field engine). Device testing confirmed the guard passes during field gameplay; the exclusion direction was apparently never tested. Print the flag inside an Impact battle to settle it. |
| M9 | M | review | `src/ui/ui_prompt.cpp:264-267,438-448` | User callbacks run while holding `prompt_state.mutex`; a cancel action that touches the prompt API self-deadlocks. The confirm/cancel button paths already drop-lock-then-call; these two diverged. |
| M10 | M | review | `patches/widescreen.c:140,144` | Karakusa widescreen patch uses the hikimaku's static; its own `previous_aspect_ratio` is declared and never used, so a runtime aspect change leaves whichever background runs second with a stale scale. |

## L — long tail

| # | Verified | Location | Issue |
|---|----------|----------|-------|
| L1 | review | linker flags | `--unresolved-symbols=ignore-all`: a typo'd symbol resolves to 0 and links clean; `syms.ld` drift dispatches the wrong host function with no build-time signal. Add a post-link `nm` check for unresolved/zero symbols. |
| L2 | review | asset extraction | Swallowed `IOException` → game launches against missing assets. Surface it. |
| L3 | review | ROM hash path | Hash I/O error presented to the user as "wrong ROM". Distinguish I/O failure from mismatch. |
| L4 | review | `src/audio` | `1 << skip_factor` UB after long stalls. |
| L5 | review | `controls.cpp` | Missing-return UB reachable from a hand-edited `controls.json`. |
| L6 | review | toast/indicator | `system_clock` used for toast expiry (wall-clock, not steady). |
| L7 | review | version stamp | 64-byte `.assets_version` read buffer → perpetual 45 MB re-extraction on long version names. |
| L8 | review | `build.gradle` | `versionCode` from `rev-list --count` is downgrade-fragile across history rewrites. |
| L9 | review | `patches/camera.c` | Flag bits stripped during the camera pointer swap window. |
| L10 | review | `patches/attack_move.c` | Attack-move direction survives area transitions. |
| L11 | review | `src/main/main.cpp` | `G64_COPY_GPU` env fork whose comment says "Remove before release". |
| L12 | review | various | `#if 0` blocks referencing possibly-removed functions; three large commented-out patch blocks (one preserving a transcription bug in amber). |
| L13 | review | `patches/background.c:49` | "Intentional crash" trap is probably a silent rdram write under the recomp memory model, not a trap. |
| L14 | verified | `src/ui/ui_saved_indicator.cpp` | Comment calls `ui_state_mutex` non-recursive; it's `std::recursive_mutex` (`ui_state.cpp:433`) and `draw_hook` relies on the recursion. Invariant holds; the stated premise is stale — reword to reference ordering, not the mutex type. |

## Recurring themes

- **Duplicated guest addresses with no shared header** — `0x800C7DB0`, `0x8020CA2C`, the `0x80000000..0x80800000` valid-RAM predicate (≈6 hand copies), the locomotion action-id ranges (3 copies), raw literals for addresses `variables.h` already names. A single `goemon_addrs.h` makes "correction applied to one of three copies" impossible.
- **Errors that degrade silently** — H4, H5, L1, L2, L3. The autosave lesson ("build the diagnostic that proves it reached its decision point") wants applying at the build/launch layer too.
- **Comments referencing line numbers / stale premises** — L14 and others. State ordering rules, not line numbers.
- **Shipped diagnostics / dead code** — L11, L12, L13.

## What's good (do not "simplify" away)

- `save_rollback.cpp` — temp-copy-then-atomic-rename with cleanup, plus a header documenting the accepted residual race with probability analysis and a re-examination trigger.
- Autosave slot-cursor bounds check correctly notes the only downstream check is an `assert` compiled out by `-DNDEBUG`, and says so.
- Magic constants carry provenance (Android config defaults cite profiling; camera `>>6` tied to the 0x400-period sine-table discovery).
- Java threat surface is small and honest: non-exported activities, SAF-only ROM entry, no user-controlled paths or zip extraction; release workflow verifies version against tag and checks the artifact isn't debug-signed.
