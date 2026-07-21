# Code review — 2026-07-20

Full-codebase critique of the Android-specific code we own (Java launcher layer,
host `src/`, `patches/`, CI). Upstream N64Recomp/RT64 internals were out of scope.

Findings came from three review passes (Java+CI, native `src/`, `patches/`).
**Verification column**: `verified` = I re-read the source and confirmed it here;
`review` = asserted by the review pass, not yet independently re-checked against
source. Fix nothing marked `review` without confirming it first.

Severity: **H** fix before next release · **M** soon · **L** long tail.

> **Second pass 2026-07-20:** a follow-up gap hunt (submodule forks, Java↔native
> seam, adversarial re-sweep, repo hygiene) found further issues — including a
> HIGH that shipped in v1.0.0/v1.0.1. See
> [code-review-2026-07-20-pass2.md](code-review-2026-07-20-pass2.md). The M tier
> below remains the open backlog from this pass.

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

> **Status 2026-07-20:** L1–L11 verified against source by three read-only passes
> and fixed where real (build-verified, `assembleDebug` green + the new patches
> guard exercised). L9's initial strike was **wrong** (it rebutted a claim the
> finding never made) and was reopened + hardened — see its row. L8 (versionCode scheme)
> was fixed with the maintainer's sign-off. L12–L14 triaged: L12 not-actionable
> (intentional annotated reference code), L13 comment corrected, L14 reworded.
>
> Actual location corrections found during verification: L1 is in `patches/Makefile`
> (not CMake); L11 is in `rt64_render_context.cpp` (not `main.cpp`).

| # | Verdict | Location (corrected) | Issue → resolution |
|---|---------|----------------------|--------------------|
| L1 | REAL — FIXED | `patches/Makefile:30-33` | `--unresolved-symbols=ignore-all` lets a host export missing from `syms.ld` link as address 0 → wrong host fn at runtime. Added a post-link guard: fail if any **undefined `recomp_*`** symbol exists (all are defined in `syms.ld`, so an undefined one is unambiguously the typo; game/libultra symbols stay undefined by design and aren't flagged). |
| L2 | REAL — FIXED | `AssetInstaller.java`, `LauncherActivity.java` | Extraction `IOException` was logged-and-swallowed, then the game launched against partial assets. `installIfNeeded` now returns success; `LauncherActivity.startGame()` pre-flights it (the chokepoint that *can* refuse — MainActivity can't) and shows a dialog on failure. |
| L3 | REAL — FIXED | `LauncherActivity.java:186` | A hash **I/O error** was funneled into the "Unexpected ROM" mismatch dialog whose primary action **deletes the ROM**. Now a compute failure shows a distinct "Couldn't read the ROM" dialog (Retry / Start anyway, never deletes). |
| L4 | REAL — FIXED | `src/main/main.cpp:267` | `1 << skip_factor` is UB once the backlog reaches ~3.2s (shift ≥32). Clamped `skip_factor` to 8 and used a `1u` literal. |
| L5 | REAL — FIXED | `src/game/input.cpp:693,723` | `get_input_analog`/`get_input_digital` switch on an untrusted `input_type` from `controls.json` with no `default` → fall off the end (UB) on an out-of-range value. Added `default`/trailing returns (treat unknown as unbound). |
| L6 | REAL — FIXED | `ui_saved_indicator.cpp:33`, `ui_state.cpp:585` | `system_clock` used to measure elapsed expiry (toast + menu key-repeat) → a wall-clock jump misbehaves. Switched both to `steady_clock`. |
| L7 | PARTIAL — FIXED | `AssetInstaller.java:86` | Fixed 64-byte single `read()` of `.assets_version` would truncate a >64-byte version and force 45 MB re-extraction every launch. Unreachable with semver tags, but read the whole file now (`Files.readAllBytes`) — also removes the single-`read` assumption. |
| L8 | PARTIAL — FIXED | `android-release.yml:48`, `build.gradle:39` | `versionCode = git rev-list --count` was downgrade-fragile if history is ever rewritten (this repo has done a PII scrub). Now tag-derived: `major*10000+minor*100+patch`, computed in the release workflow, deterministic and rewrite-proof. v1.0.0 shipped 661; any release ≥ v1.0.1 → ≥ 10001, so the switch is monotonic with no transition scheme. The workflow now also **rejects** non-`vMAJOR.MINOR.PATCH[-suffix]` tags and minor/patch ≥ 100. `-rc` tags intentionally share their final release's code (dry-run sideloads). |
| L9 | REAL (window) — HARDENED | `patches/camera.c:502,588` | **Correction of an earlier wrong strike.** The finding was about the *swap window*, not the restore: between the swap and the verbatim restore, the live `node+0x2C` word holds the raw replacement pointer with flag bits (bit 0, 28-30) cleared, and `func_801CE4D0` runs on it. The first pass dismissed this by proving the restore is verbatim — which the finding never disputed. The only *known* reader in the window re-masks with `& 0x8FFFFFFE`, so it wouldn't observe the loss, but "the known consumer masks" ≠ "nothing branches on those bits" (the finding's explicit, unproven point). Rather than rest on that, both swaps now OR the original word's flag bits into the replacement (`| (old_cam & ~0x8FFFFFFEu)`), making the node word bit-faithful during the window. Zero risk (masking readers strip exactly those bits). |
| L10 | REAL — FIXED | `patches/attack_move.c:116` | Frozen lunge direction + `g_have_move` were never reset on area transition → first post-transition attack could lunge the wrong way. Added a map-id guard (`0x800C7AB2`, mirroring `camera.c`) that drops the held direction and the sample anchor on area change. |
| L11 | REAL — FIXED | `rt64_render_context.cpp:353` | `G64_COPY_GPU` env fork (marked "Remove before release", shipped anyway) let an env var flip a render path in release builds. Removed the fork; hardcoded the shipping default (`copyWithGPU = false`). |
| L12 | NOT-ACTIONABLE | `patches/main.c:13`, `input.cpp:580`, `anime.c:449`, `tagging.c:3,35` | The two `#if 0` blocks are intentional/parked (a documented "quicksave disabled for now" path; a debug step-speed toggle). The three `/* ... */` blocks are **annotated reference reimplementations** (`/* Not needed */`, `/* Not needed? */`, `/* Wrong? */` — the last is the transcription-bug-in-amber). Each states why it's disabled; in a project this RE-heavy, deleting them destroys context. Left as-is by choice — not accidental cruft. |
| L13 | REAL — COMMENT FIXED | `patches/background.c:49` | The "intentionally crash … triggering the debugger" write to `-1` does NOT trap under the static recompiler — the guest store to `0xFFFFFFFF` is masked into RDRAM and silently writes high memory. Corrected the comment to say so and flagged a TODO to route to a real host abort. Behavior left unchanged: it's an unreached "should not happen" error path, and choosing crash-vs-continue isn't safe to decide statically without device repro. |
| L14 | verified — FIXED | `ui_saved_indicator.cpp:17`, `ui_state.cpp:562`, `CLAUDE.md` | Both comments (and CLAUDE.md) called `ui_state_mutex` "non-recursive → self-deadlock". It's actually `std::recursive_mutex` (`ui_state.cpp:433`) that `draw_hook` **re-enters** at `:738`, so re-locking never self-deadlocks — the deadlock rationale was false. Reworded all three to the real reason: the tick must precede the launcher-return check (`:569`) so an expiring toast updates `is_any_context_shown()` before it's read. No behavior change. |

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
