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

Still open from this round, both deliberate:

- The `cvgAdd` breakage is still reasoned, never observed. The cheap way to
  close it is `ubershadersOnly = true` on the A15 in a scene using coverage
  wrap, which forces the worst case permanently instead of transiently.
- No CI guard against a future unguarded `SRC1_`.

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
- **No CI guard.** A future unguarded `SRC1_` reintroduces this silently. The
  repo has precedent for this kind of check (`patches/Makefile`'s undefined
  `recomp_*` symbol guard).
- **Size cost:** ~1.3 MB of extra SPIR-V, uncompressed.

## Related reading

- `docs/re-notes/RESUME-mali-issue15.md` — the full investigation, the device,
  the validation-layer setup, and everything refuted along the way
- CLAUDE.md § "Current focus" — the authoritative issue #15 bullet
- Memory `goemon-issue-15-mali-white-screen`, `driver-success-is-not-validity`,
  `mali-repro-device-a15`, `device-install-method`
