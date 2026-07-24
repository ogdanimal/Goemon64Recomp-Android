# RESUME — waiting on review of the issue #15 Mali fix

Handoff note for a fresh session. Written 2026-07-24.

**You are in a WAITING state. The work is done; the maintainer is reviewing it.
Do not start the downstream steps on your own initiative.**

## Where things stand

The issue #15 white-screen-on-Mali fix is **complete, device-verified on both
GPU vendors, and CI-green**. It was handed to the maintainer for review at the
end of the 2026-07-23 session, along with an in-depth technical rundown.

- `dev` tip **`b2c053b`**, in sync with `origin`, working tree clean
- submodule tips: rt64 **`3606f0b`**, plume **`4e77e67`** (both fork-pushed and
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
  backends hardcode the capability true. An assert would be cheap insurance.
- **`assert(!blob.empty())` compiles out in release** — a parse failure would
  yield a null shader silently. Pre-existing pattern, now with four more blobs.
- **Coverage fidelity is characterised from the code, not measured.** Nobody has
  compared Mali output against Adreno side by side. Worst case is
  `cvgAdd && alphaBlend`, where coverage-wrap emulation accumulates the blend
  factor instead of coverage — broken there, not merely approximate.
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
