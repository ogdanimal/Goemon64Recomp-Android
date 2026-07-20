# Android profiling plan

Written 2026-07-19. Status: **planned, not started — deliberately post-release work** unless the
Phase 0 baseline shows the game is unplayably slow on target hardware (that's a product call that
can be made from the baseline capture alone).

> **Update 2026-07-19:** the menu-framerate branch of this plan was picked up early (a reproducible
> ~14 FPS on the Select Adventure Diary screen). Findings — measured decomposition, root cause, and
> the fix ledger — are in [`android-profiling-results.md`](android-profiling-results.md). The
> general baseline sweep below is still unstarted.

Goal: determine where frame time actually goes on the test phone, rank the bottlenecks, and only
then pick optimizations. The guiding question for every step is **"CPU-bound or GPU-bound, and on
which thread/pass?"**

Context: the 2026-07-19 review of the plume Vulkan layer (`lib/rt64/src/contrib/plume`, branch
`goemon-android`) found no performance problems in the layer itself on healthy devices. The only
perf-relevant review finding (unthrottled per-bind logging in `setPipeline`/`setDescriptorSet`)
matters only on devices where pipeline/descriptor creation already failed.

## Phase 0 — Setup and baseline

1. **Commit the two diagnostic hunks in `plume_vulkan.cpp`** (chosen-GPU + loader-version logging).
   They identify the device from any logcat capture — step one of profiling, already written.
2. **Pick 2–3 repeatable test scenes**: a representative playable area, a heavy
   framebuffer-effects scene (the file-select menus needed an rt64 framebuffer fix, so they're a
   candidate), and one long-play scene for thermal testing.
3. **Capture a baseline**: a play session with logcat saved, plus frame timing via
   `adb shell dumpsys SurfaceFlinger --latency` or a Perfetto trace. Record steady-state FPS *and*
   the spike pattern — steady low FPS and periodic hitches have different causes.

## Phase 1 — Coarse triage (no code changes)

The in-game settings already expose the experiment knobs (resolution tiers Original 1x–4x /
integer-scale, downsampling, MSAA 2x/4x/8x — see `src/main/rt64_render_context.cpp`):

- **Resolution sweep**: FPS scales strongly with resolution → fragment/bandwidth-bound (the usual
  mobile answer). FPS barely moves → CPU-bound; GPU tweaks are wasted effort.
- **MSAA off vs 2x**: MSAA is cheap on tile-based GPUs *unless* rt64's pass structure forces
  mid-frame tile stores/resolves — a big differential here is a smoking gun for pass-structure
  problems.
- **Thermal check**: minute-1 vs minute-15 FPS. Adreno throttling can masquerade as a rendering bug.
- **Perfetto trace** of the baseline scene: which thread saturates (recomp CPU core, RSP, gfx,
  "RT64 Present"), GPU frequency, jank attribution.

## Phase 2 — GPU deep dive

- **RenderDoc for Android** frame anatomy: render passes, draws, framebuffer copies per frame.
  Plume's framebuffers use `VK_ATTACHMENT_LOAD_OP_LOAD` on color attachments, which forces a full
  tile load every pass on a tiler; N64 framebuffer-effect emulation multiplies passes. Pass count ×
  load/store cost is the prime GPU-side suspect (rt64 was designed for desktop immediate-mode GPUs).
- **Snapdragon Profiler or Android GPU Inspector** counters: is the limiter bandwidth, ALU, or
  texture; directly measures tile load/store traffic.

## Phase 3 — In-engine instrumentation (only if Phase 2 leaves questions)

Plume already has `VK_QUERY_TYPE_TIMESTAMP` query-pool support and rt64's workload/present queues
are cleanly separated. Add per-stage GPU timestamps (workload render, post-processing, RmlUi,
present) logged once per second to logcat behind an `__ANDROID__` debug flag — same pattern as the
existing `[plume]` diagnostics (stderr reaches logcat via the `Goemon64-stdio` pump in
`src/main/main.cpp`).

## Phase 4 — Codebase-specific hypotheses to test against the data

- **High-precision framebuffer**: `high_precision_fb_enabled = shaderLibrary->usesHDR`
  (`rt64_render_context.cpp`); 16-bit-float targets double bandwidth on mobile — measure with it off.
- **Shader-compile hitches**: rt64 compiles specialized pipelines at runtime; Adreno pipeline
  creation is slow. If the complaint is stutter rather than low FPS, this is the likely cause
  (Perfetto spikes should correlate with new-pipeline creation).
- **Present pacing**: Android is effectively locked to FIFO (Mailbox reverted upstream, IMMEDIATE
  rarely supported) and `presentWait` is likely unsupported — check what frame pacing falls back to.

## Division of labor

- **User (on-device)**: capture sessions, installing RenderDoc / Snapdragon Profiler, the
  settings-sweep play sessions.
- **Claude**: instrumented APK builds, timestamp instrumentation, analysis of
  logcat/Perfetto/RenderDoc captures.

Results land as a doc next to this one with baseline numbers and a ranked bottleneck list.

## Starting move (when picked up)

Commit the diagnostics → one play session over the test scenes with logcat recording → let the
baseline numbers pick which Phase 2 branch matters.
