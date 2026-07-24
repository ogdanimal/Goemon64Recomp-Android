# RESUME — waiting on review of the issue #15 Mali fix

Handoff note for a fresh session. Written 2026-07-24.

**You are in a WAITING state. The work is done; the maintainer is reviewing it.
Do not start the downstream steps on your own initiative.**

## Where things stand

The issue #15 white-screen-on-Mali fix is **complete, device-verified on both
GPU vendors, and CI-green**. It was handed to the maintainer for review at the
end of the 2026-07-23 session, along with an in-depth technical rundown.
A first round of review feedback came back and was applied — see "Review round 1"
below; the fix itself is unchanged and the wait continues.

- `dev` tip: check it, do not trust a hash written here (it drifts every commit)
- submodule tips: rt64 **`c6ea60d`**, plume **`4e77e67`** (both fork-pushed and
  `ls-remote`-verified; the whole gitlink chain is CI-buildable)
- CI green. The two original fix-commit run records (`30053393161`,
  `30055908366`) were **deleted** on 2026-07-23 during the PII cleanup — they
  published pre-rewrite commit SHAs. Those commits no longer exist on `dev`
  anyway; the rewritten history has its own green run. Re-check with
  `gh run list --repo ogdanimal/Goemon64Recomp-Android --branch dev`.
- `main` is **behind** `dev` and does NOT carry the fix — deliberately

## Do NOT do these without the maintainer saying so

- Fast-forward `main`
- Tag anything (`v1.0.3`, `v1.0.3-rc1`, …)
- Reply to GitHub issue #15
- Start the two open Vulkan-validation findings
- "Improve" the fix speculatively

All of that is gated on the review verdict, not on anything technical. The
standing decision is still: **hold the issue #15 reply until the fix ships.**

## If the maintainer comes back with changes

1. Apply them on `dev`, through the submodule chain: plume → rt64 → root, each
   pushed to its `goemon-android` fork branch, then verify every gitlink matches
   a pushed remote tip with `git ls-remote` before trusting it. A gitlink bumped
   to a commit that only exists locally is unbuildable by CI.
2. **Re-verify on the Mali device for any shader or blend change.** Connection
   details, the validation-layer toggle and the gotchas are in
   `docs/re-notes/RESUME-mali-issue15.md`.
3. **If the change touches the `alphaBlend` agreement, use the positive
   control.** Set `ubershadersOnly = true` in `lib/rt64/src/hle/rt64_workload_queue.h:83`
   in a local build. With a broken fallback the title screen loses its entire
   opaque background and only the alpha-blended logo survives; with a correct
   one the background renders. Revert that line afterwards — it is a diagnostic,
   never commit it.

## If the review is approved

Follow NEXT ACTIONS in CLAUDE.md § "Current focus" from step 2 onward
(fast-forward `main` → `v1.0.3-rc1` dry run → `v1.0.3` → reply to issue #15).

## The one-paragraph version of what is being reviewed

RT64 does N64 alpha blending entirely with dual-source blending — the blend
factor rides in `SV_TARGET1` so `SV_TARGET0.a` can carry N64 coverage. No Mali
GPU supports `dualSrcBlend`, and the Mali driver returns `VK_SUCCESS` for the
resulting invalid pipeline and then blends undefined, which is why the screen
was white with nothing in any log. The fix adds a `dualSrcBlend` device
capability to plume and a `NO_DUAL_SRC_BLEND` shader variant that carries the
factor in the primary output's alpha, selected at runtime, so hardware that
supports dual-source takes the byte-identical old path. The unavoidable cost is
that the fallback loses the coverage value wherever the factor takes its place.

## Review round 1 (2026-07-23) — applied

Feedback came back on the review itself. Every claim was checked against the
code before acting; two were real and had not been recorded anywhere.

- **The tradeoff description was stale and too narrow.** It said `cvgAdd`
  without `alphaBlend` is unaffected. True of the SPECIALIZED path only — the
  ubershader path writes the factor unconditionally (that is what the follow-up
  fix does), so it loses coverage for every uber draw regardless of
  `alphaBlend`. Corrected in CLAUDE.md and in the weak point below.
- **The lost alpha reaches emulated RDRAM, not just GPU-side coverage.**
  `Float4ToRGBA16` (`lib/rt64/src/shaders/Formats.hlsli:95`) derives the RGBA5551
  alpha bit from `round(a * 255) % 8` and `FbWriteColorCS` packs it back, so the
  game can read a blend factor where coverage should be. Whether mnsg reads
  those bits is unknown. Recorded in CLAUDE.md.
- **DXIL/Metal assert added** (rt64 `c6ea60d`) — see the weak point below.
- **Line cite drift:** the ubershader `alphaBlend = true` hardcode is at
  `rt64_raster_shader.cpp:516`, not `:510`. Cite the function, not the line.
- **Not confirmed:** a bad `dev` hash (`03f9f00`) was reported as needing a doc
  fix. It never entered any document — `grep` finds it in no `*.md`. Nothing to
  do. Several other "gaps" were already listed as known weak points below.

Also closed in this round: the CI guard (see the weak point below).

Still open:

- ~~The `cvgAdd` breakage is still reasoned, never observed.~~ **MEASURED
  2026-07-23 — see "The cvgAdd measurement" below.** The worst case IS reachable;
  the counter that proved it is committed (rt64 `6a7d0be`), disabled. Procedure
  to re-run it:

  1. Set `RT64_DIAG_CVG_ADD` to `1` in
     `lib/rt64/src/render/rt64_framebuffer_renderer.cpp` (near the top) and build
     a debug APK. Never commit it enabled.
  2. Install, play through the intro, title screen and some real gameplay, and
     watch `adb logcat | grep cvgdiag`. It prints every 50000 draws:
     `draws=N wrap=N(blend/plain) save=N(blend/plain)`.
  3. **`wrap` and `save` both zero across a real session ⇒ the worst case is
     unreachable in this game** and the tradeoff is confined to ordinary
     alpha-blended coverage, which already renders correctly on device. That
     closes it without a Mali session — the counts come from the game's display
     lists, so ANY device or backend gives the same answer.
  4. **Non-zero `blend` counts ⇒ do the visual A/B on the A15** with
     `ubershadersOnly = true`, which forces the worst case permanently instead of
     transiently, and compare against Adreno.

  Note the counter is device-independent but the *consequence* is not: only the
  Mali fallback path suffers it, so a non-zero count is a reason to look at Mali
  output, not evidence that anything is wrong on Adreno.

## The cvgAdd measurement (2026-07-23)

Ran `RT64_DIAG_CVG_ADD` on both GPU vendors through the intro attract sequence,
at default graphics settings, validation layers off.

| device | capability | at N draws | `cvgDst=WRAP` | `cvgDst=SAVE` |
|---|---|---|---|---|
| A15, Mali-G57 | `dualSrcBlend=0` (fallback) | 550k | 6366, **all alpha-blending** | 99, **all alpha-blending** |
| RP5, Adreno 650 | `dualSrcBlend=1` (dual source) | 500k | 7170, **all alpha-blending** | 160, **all alpha-blending** |

What this establishes:

- **The worst case is reachable.** `cvgAdd` draws happen in the thousands in the
  intro alone, so "cvgAdd && alphaBlend is broken on the fallback" is a real
  code path in this game, not a theoretical corner.
- **`cvgAdd && alphaBlend` is not a corner of a corner — in everything measured
  so far it is the ONLY case.** The `plain` (non-alpha-blending) count was **zero
  everywhere on both devices**, so across the scenes captured the benign half of
  the tradeoff never actually occurs. Scope that honestly: the measurement is one
  intro attract sequence on each device. Effect-heavy gameplay (explosions,
  water, shadows) is where a non-blending `cvgAdd` draw would first plausibly
  appear, and none of it was exercised.
- **The two devices agree on the draw mix.** Both vendors produce non-zero counts
  of the same kind, and the totals differ only because the runs are not
  frame-synced (different resolution, framerate and time per scene). This is
  worth stating only as a sanity check on the counter — that the draws come from
  the emulated game rather than the GPU was never in doubt. It says nothing about
  the cross-device question that matters, which is whether the fallback *renders*
  equivalently; that remains untested.

What it does NOT establish, and why the question is not fully closed:

- **No same-frame comparison was made.** The two captures are different moments
  of the cutscene, so nothing here rules out a subtle difference.
- A **positive control was run**: `ubershadersOnly = true` on the Mali device,
  which loses coverage on EVERY draw rather than only the transient ones. The
  intro still rendered correctly (sky gradient, character, roof, text all
  correctly composited) with `wrap` draws confirmed occurring in that same run.
  That is real evidence the loss is not grossly visible — but it is visual
  inspection at screenshot scale, which is exactly what missed the ubershader
  bug earlier. Subtle AA-edge differences would not show up this way.
- **Only the intro was exercised.** No gameplay, no effect-heavy scenes.

Standing conclusion: the tradeoff is real and reachable, has no gross visual
impact on the scenes tested, and remains the first suspect if a Mali user reports
edge or transparency artifacts.

## Review round 2 (2026-07-23) — APPLIED 2026-07-24

Feedback on the round-1 report. Four substantive points; all four were checked
against the code and **all four are correct**. None blocks anything gated.

**Note on the review's own base:** it verified against root commit `2b81c8e`,
which no longer exists — `dev` history was rewritten after it (PII scrub). Its
findings still hold because the content it read is unchanged, but re-derive any
hash from the tip.

1. **The 390 vs 6366 wrap-count gap was unexplained.** Answered here, from the
   captured logs: the two numbers are at **different sample points**, not
   different scene coverage. The counter prints every 50000 draws; 6366 was the
   reading at `draws=550000` in the normal run, 390 was the reading at
   `draws=50000` in the control run. At the **matched** 50000-draw sample the
   normal run had `wrap=0` and the control had `wrap=390`, i.e. the control
   reached coverage-wrap draws *earlier*, not less. So the control was not
   running blind footage. What remains genuinely unproven is whether the
   screenshot moment coincides with a wrap-heavy moment — the counts prove wrap
   draws occurred during the run, not that they were on screen in that frame.
2. **"It is the only case" overstates intro-scoped data.** Correct, and it
   contradicts this doc's own "intro only" caveat. The defensible claim is "in
   everything measured so far". Effect-heavy gameplay (explosions, water,
   shadows) is where a non-blending `cvgAdd` draw would first plausibly appear.
   **Fix the wording in CLAUDE.md and here when applying this round.**
3. **"Device independence is now observed" answers a question nobody asked.**
   Fair. What the two devices agree on is the *draw mix*, which was never in
   doubt since the draws come from the emulated game. The cross-device question
   that matters — does the fallback *render* equivalently — is exactly the one
   still untested.
4. **Three guard-script edge cases, all confirmed present:**
   - Multi-line ternary (`= dualSrcBlend\n ? SRC1_ALPHA`) fails **closed** — a
     false positive, and the message will not hint that reformatting caused it.
   - The shader check is hardcoded to `RasterPS.hlsl` (`:26`), so a NEW shader
     using `SV_TARGET1` / `vk::index(1)` escapes entirely. The C++ half is
     repo-wide; the output half is not.
   - The awk walker has no `#elif` rule, so a secondary output in an `#elif`
     branch following a guarded `#if !defined(NO_DUAL_SRC_BLEND)` counts as
     gated.

### What was applied

Points 1–3 were doc edits (this file, CLAUDE.md, and the issue-15 memory): the
"only case" claim now reads "in everything measured so far" with the intro-only
scope stated in the same breath, and the device-independence bullet is requalified
to what it actually shows — the draw mix agrees, the rendering comparison is still
untested. Point 1 needed no change beyond the answer already written above.

Point 4 rewrote `.github/scripts/check-dual-src-blend.sh`:

- **Check 1 is statement-scoped, not line-scoped.** An awk walker accumulates
  text since the last `;`/`{`/`}` and requires `dualSrcBlend` somewhere in the
  statement containing the `SRC1_` use, so a reformatted ternary passes. A gate in
  an *enclosing* `if ()` still fails — deliberately conservative — and the message
  now says which is which.
- **Check 2 scans every `*.hlsl` / `*.hlsli` under `lib/rt64/src`** (contrib
  pruned), not just `RasterPS.hlsl`, which stays as the preflight anchor. A new
  shader declaring a secondary output can no longer escape.
- **The awk walker has an `#elif` rule.** `#elif` clears the guard at that depth
  unless the new condition re-establishes it (`#elif !defined(NO_DUAL_SRC_BLEND)`),
  the same as `#else`.
- **Deliberately still out of scope, now documented in the script header:** the
  HLSL text `generateShaderText` builds in C++ names `SV_TARGET1` unconditionally.
  That feeds DXIL/Metal only, both of which hardcode the capability true, and
  rt64 asserts `shaderFormat == SPIRV || capabilities.dualSrcBlend` before
  dispatching there. Policing it here would mean allowlisting the exact line this
  guard forbids, so the assert covers it instead.

**Provoked, not assumed** — before the change all three new cases behaved exactly
as the review said (multi-line ternary → 2 false failures; new shader with a bare
`SV_TARGET1` → clean pass; `#elif` branch → counted 2/2 gated). After: ternary
passes, new shader fails, `#elif` fails, and `#elif !defined(NO_DUAL_SRC_BLEND)`
still passes. The original five modes were re-provoked and all still fire
(missing submodule, missing anchor shader, bare `SRC1_`, secondary output in
`#else`, and both zero-site vacuous-pass guards), plus the new
gate-in-enclosing-`if` case. Fixtures: minimal repo roots with the two real
source files copied in and mutated.

## Review round 3 (2026-07-24) — APPLIED, and both open questions ANSWERED

The reviewer re-derived everything from the new tip and re-provoked the round-2
guard fixes independently rather than trusting the provocation table. All four
behaved as reported. Two new probes then found residual false negatives.

**Both were regressions round 2 introduced** — verified by re-running each probe
against the pre-round-2 script (`f6409b5`), which caught both. Fixing two false
positives had opened two false negatives, in the fail-open direction:

1. **Compound fallback condition.** `is_negated_guard` tested for the macro name
   and for a `!defined` *independently*, so
   `#if defined(NO_DUAL_SRC_BLEND) && !defined(FOO)` counted as gated — a
   secondary output in the FALLBACK branch, exactly what check 2 exists to
   forbid. The pre-round-2 code required `!defined` adjacent to `#if`, so the
   refactor to a helper is what lost it. Fixed: the negation must apply to
   `NO_DUAL_SRC_BLEND` itself, and `||` (which can widen a guarded region back
   into fallback builds) disqualifies the condition.
2. **Comments inside the statement window.** Widening check 1 from the line to
   the statement also widened what could sit in it, so a
   `// dualSrcBlend chooses this` above a bare `SRC1_ALPHA` read as the gate — a
   *wider* aperture than the line-based check it replaced. Fixed: both checks
   strip `//` and `/* */` (tracked across lines) before matching.

**Three more holes were found by writing the suite, present since the ORIGINAL
guard and missed by all three review rounds:** a trailing `// dualSrcBlend` on
the same line as a bare factor, `||` widening a guarded region, and
`NO_DUAL_SRC_BLEND_EXTRA` matching as a prefix.

**`.github/scripts/test-dual-src-blend-guard.sh` (new, wired into both
workflows).** 27 cases, both directions, against synthesized fixtures so it runs
without the submodule. Proven live rather than assumed: run against the round-2
guard it fails on exactly the 7 known holes; against the original guard it fails
on 8, including that round's false positives. **This is an addition beyond what
the review asked for** — the review asked for two regexes. The argument for it is
that this guard has now been hardened three times and two of those rounds moved a
hole rather than closing one, which is the failure a suite exists to stop. Drop
it if the extra CI surface is not wanted; the regexes stand alone.

### The two open questions — answered by the reviewer

- **`generateShaderText` exclusion: ACCEPTED as made.** Allowlisting the one line
  containing the token the guard forbids would teach the script to ignore its own
  target; the invariant belongs at the semantic layer where the assert puts it.
  **Caveat to hold knowingly:** `assert` compiles out under `NDEBUG`, so release
  D3D12/Metal builds do not check it. Acceptable today because both backends
  hardcode the capability `true` at compile time, so it can only fire if someone
  deliberately changes plume — and that person will run a debug build eventually.
  If it ever needs to be hard, promote it to an unconditional check that fails
  pipeline creation loudly. **Not required for this release.**
- **Render-equivalence testing: NOT a ship blocker — accepted known limitation.**
  The Mali baseline is a white screen, so the fallback strictly dominates the
  status quo on affected hardware, and the dual-source path is byte-identical for
  every currently-working device, so regression risk to existing users is zero.
  What is unproven is subtle (AA-edge/coverage equivalence on draws the
  measurements show always alpha-blend anyway) and the positive control bounds
  the damage at "not grossly visible". **Conditions attached to shipping:** name
  the limitation in one sentence in both the release notes and the issue #15
  reply (coverage-based effects are approximated on GPUs without dual-source
  blending), and keep the same-frame Mali/Adreno capture plus effect-heavy
  gameplay as **post-release** validation — that evidence is wanted for any
  upstream PR regardless, and the `RT64_DIAG_CVG_ADD` counter makes triage cheap
  if a Mali user reports artifacts.

**RELEASE DECISION 2026-07-24: HELD.** The maintainer declined the ship-now
recommendation and chose to measure render equivalence first. The reviewer's
conditions still apply to the eventual release; what changed is that the capture
is now a **pre-release gate**.

## The render-equivalence session (the current gate)

**The question:** does the `NO_DUAL_SRC_BLEND` fallback render equivalently to
the dual-source path, on content where the lost coverage value would show?

**Design note that matters more than the rest of this plan: do NOT make the
primary experiment a cross-device Mali-vs-Adreno comparison.** Two different
GPUs, drivers and rasterizers cannot produce identical frames even when the fix
is perfect, so every difference found would be unattributable and the test could
not fail cleanly. It confounds the variable of interest with the hardware.

**Do this instead — same-device A/B on the RP5 (Adreno 650):**

1. Add a debug-only override that forces `capabilities.dualSrcBlend = false` on a
   device that supports it (a `#define` next to `RT64_DIAG_CVG_ADD`, same
   never-commit-enabled rule). Adreno reports `dualSrcBlend=1`, so this yields
   dual-source vs fallback on identical hardware, identical driver, identical
   frame pacing — **the shader path becomes the only variable**, which is exactly
   what "renders equivalently" means and what the cross-device version cannot
   isolate.
2. Capture the **intro attract sequence**, which is deterministic from boot, so
   the same frame really is comparable between runs. Anchor captures to a
   repeatable marker (a logcat line, or a fixed draw count from the diag counter)
   rather than wall-clock guessing.
3. Compare pixel-wise and **quantify** — max/mean channel delta and a difference
   image, not "looks the same". Screenshot-scale eyeballing is what missed the
   ubershader bug; see the positive-control note above.
4. Expect *some* difference wherever coverage genuinely differs. The question is
   whether it is confined to AA edges at low amplitude, or structural.

**Then the scoping question, separately:** run `RT64_DIAG_CVG_ADD` through
**effect-heavy gameplay** (explosions, water, shadows) rather than the intro, and
watch for a non-zero `plain` (non-alpha-blending) count. That is the measurement
that would either confirm or break the "in everything measured so far it is the
only case" claim. It is device-independent, so the RP5 alone answers it.

**The A15 keeps one job:** re-confirm the fallback still renders correctly on
real Mali after any change made for this session. The equivalence question itself
does not need it.

**Prerequisites:** the RP5 (uninstall + checksum-verified save restore first if a
release-signed build is on it — see § Environment), and for step 4 the A15 over
wireless debugging with its rotating port re-derived. Both devices' gotchas are
in `RESUME-mali-issue15.md` and memory `mali-repro-device-a15`.

## Known weak points the review may land on

These are the things flagged as worth scrutiny — expect feedback here:

- **The `alphaBlend` agreement invariant.** `PipelineCreation::alphaBlend` and
  the shader's own `alphaBlend` must agree. They do on the specialized path
  (same predicate) and deliberately do NOT on the ubershader path, which
  hardcodes `true` — handled by the `DYNAMIC_RENDER_PARAMS` branch. Nothing
  enforces this; it is comments on both sides. **This already caused one bug**
  (see the follow-up-bug bullet in CLAUDE.md).
- **The fallback is SPIR-V only.** The DXIL (`generateShaderText`) and Metal
  paths still emit/select dual-source unconditionally, safe *only* because those
  backends hardcode the capability true. **Addressed 2026-07-23 (rt64 `c6ea60d`):**
  both `RasterShader` and `RasterShaderUber` now assert
  `shaderFormat == SPIRV || capabilities.dualSrcBlend` before the format
  dispatch (`rt64_raster_shader.cpp:110` and `:458`). It documents the coupling
  and fails loudly in a development build — it does not add a fallback, and it
  compiles out under `NDEBUG`. **Reviewer-accepted round 3**, with that NDEBUG
  gap held knowingly; promote it to an unconditional pipeline-creation failure
  only if it ever needs to be hard.
- **`assert(!blob.empty())` compiles out in release** — a parse failure would
  yield a null shader silently. Pre-existing pattern, now with four more blobs.
- **Coverage fidelity is characterised from the code, not measured.** Nobody has
  compared Mali output against Adreno side by side. Worst case is `cvgAdd`, where
  coverage-wrap emulation accumulates the blend factor instead of coverage —
  broken there, not merely approximate. **Corrected 2026-07-24:** this used to
  say `cvgAdd && alphaBlend`. That holds on the specialized path only; the
  ubershader path writes the factor unconditionally, so it loses coverage for
  every uber draw. The lost alpha also reaches emulated RDRAM through
  `Float4ToRGBA16` → `FbWriteColorCS` (the RGBA5551 alpha bit), so the game
  itself can read it. See the tradeoff bullet in CLAUDE.md § "Current focus".
- **One Mali device, one driver** (G57 / r38p1). The reporter's G77 is
  unconfirmed and can only be confirmed by shipping and asking.
- **No CI guard.** ~~A future unguarded `SRC1_` reintroduces this silently.~~
  **CLOSED 2026-07-23:** `.github/scripts/check-dual-src-blend.sh`, run early in
  both workflows. It requires every `SRC1_` factor in rt64's own C++ to be chosen
  in a statement naming `dualSrcBlend`, and every `SV_TARGET1` / `vk::index(1)`
  in any rt64 shader source to sit inside `#if !defined(NO_DUAL_SRC_BLEND)`. It
  fails if it finds neither, so deleting the dual-source path cannot make it pass
  vacuously — the soft spot the `recomp_*` symbol guard has. Verified by
  provoking every failure mode, not just by watching it pass. **Widened
  2026-07-24** by review round 2: statement-scoped C++ matching, all shader files
  rather than only `RasterPS.hlsl`, and an `#elif` rule — see "What was applied"
  above.
- **Size cost:** ~1.3 MB of extra SPIR-V, uncompressed.

## Related reading

- `docs/re-notes/RESUME-mali-issue15.md` — the full investigation, the device,
  the validation-layer setup, and everything refuted along the way
- CLAUDE.md § "Current focus" — the authoritative issue #15 bullet
- Memory `goemon-issue-15-mali-white-screen`, `driver-success-is-not-validity`,
  `mali-repro-device-a15`, `device-install-method`
