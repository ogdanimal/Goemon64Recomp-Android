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
- CI green on both fix commits (`30053393161`, `30055908366`)
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
- **`cvgAdd && alphaBlend` is not a corner of a corner — it is the ONLY case.**
  The `plain` (non-alpha-blending) count was **zero everywhere on both devices**.
  Every single coverage-wrap draw this game issues also alpha-blends, so the
  benign half of the tradeoff never actually occurs.
- **Device independence is now observed, not just argued.** Both vendors produce
  non-zero counts of the same kind. The totals differ only because the two runs
  are not frame-synced (different resolution, framerate and time per scene), not
  because the GPU changes what the game draws.

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
  dispatch. It documents the coupling and fails loudly in a development build —
  it does not add a fallback, and it compiles out under `NDEBUG`.
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
  on a line naming `dualSrcBlend`, and every `SV_TARGET1` / `vk::index(1)` in
  `RasterPS.hlsl` to sit inside `#if !defined(NO_DUAL_SRC_BLEND)`. It fails if it
  finds neither, so deleting the dual-source path cannot make it pass vacuously
  — the soft spot the `recomp_*` symbol guard has. Verified by provoking all
  five failure modes, not just by watching it pass.
- **Size cost:** ~1.3 MB of extra SPIR-V, uncompressed.

## Related reading

- `docs/re-notes/RESUME-mali-issue15.md` — the full investigation, the device,
  the validation-layer setup, and everything refuted along the way
- CLAUDE.md § "Current focus" — the authoritative issue #15 bullet
- Memory `goemon-issue-15-mali-white-screen`, `driver-success-is-not-validity`,
  `mali-repro-device-a15`, `device-install-method`
