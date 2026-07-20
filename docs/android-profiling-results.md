# Android profiling results — menu framerate

Companion to [`android-profiling-plan.md`](android-profiling-plan.md). This doc records the first
real capture: the **Select Adventure Diary** screen runs at ~14 FPS on the test device, and we now
know why, in measured detail.

Status: **diagnosis confirmed and decomposed.** Fix (b) precise-barrier is crossed off (18 genuine
deps). Fix (a) `copyWithGPU=true` is crossed off **as a serialization cure** — but it is a **real
+24% mitigation** (14.0→17.4 FPS, glitch-free on the worst screen), so it is *retained as the cheapest
evidenced smoothness lever for this game*, pending a copies-on validation pass on gameplay/other
screens. (An earlier revision wrote (a) off entirely; corrected 2026-07-19.) The ~46ms `gpuWait` is
~18 genuine serialized framebuffer-as-texture deps at min GPU clock, so no fence-policy change cures
it. **Remaining levers: (a) copies-on mitigation, DVFS/clock, (c) cut the pair count [engine].** UPDATE
2026-07-19 (later session): the desktop rig is now working and **(c) has been decomposed** — the 20
pairs are a ~6×-repeated color-cycling background effect that ping-pongs the render target; see
"Option-(c) RE: the 20 pairs decoded" below. DVFS/clock is still a pending firmware-mode capture.

## Provenance

- **Device**: Retroid Pocket 5 (Snapdragon 865 / Adreno 650), 1080×1920 @ 60 Hz, **not rooted**.
- **Numbers below** come from the `[g64prof]` instrumentation on rt64 branch `diag/menu-framerate`,
  **pushed to `fork` (ogdanimal/rt64)**: the per-second fullSync/render lines are commit `6c07782`;
  the per-pair enumeration (§"Option-(c) RE") is commit `7e49e1f`; the `[g64tile]` makeFramebufferTile
  accept/decline trace (§"Why tile copies decline") is commit `fa91fd6`. Deliberately kept off
  `goemon-android` — diagnostic scaffolding, remove-or-gate before release. The parent gitlink is NOT
  bumped to this branch (stays at `8c73b3f` goemon-android); `diag/menu-framerate` is checked out
  locally to reproduce, so CI keeps building the release line.
- **Two capture environments — do not conflate:** (1) the **device** — Retroid, arm64 `gradlew
  assembleDebug`, logcat `-s Goemon64-stdio`; authoritative for all timing. (2) the **desktop rig**
  (later session) — WSL `build-desktop` (clang, `-DRT64_PROFILE_LOGCAT=1`), **llvmpipe software
  Vulkan**; authoritative for structural *counts* only (pairs/fences/naturalFences/flushReason),
  never for timing (it emulates GPU work on CPU — e.g. copies-on looks slower there, the opposite of
  the device). `copyWithGPU=false` (shipping default) unless stated; `G64_COPY_GPU=1` flips it.
- **Capture**: steady-state samples only (first ~2 s after a scene change discarded while the profiler
  rings warm up).
- Cross-checked independently against `dumpsys SurfaceFlinger --latency` (12 FPS, vsync-quantized
  intervals) and the device's own on-screen FPS overlay (14.3).

## The two baselines

| Scene | fullSync/s | pairs/frame | fences/frame | naturalFences/frame | pairSync ms | gpuWait | cpuCopy | uploadWait |
|---|---|---|---|---|---|---|---|---|
| **Gameplay** (steady) | ~30 | 2.0 | 2.0 | 0.0 | ~4.7 | ~3.5 | 0.11 | ~0.1 |
| **Diary screen** (steady) | ~14.5 | 20.0 | 20.0 | 12.0 | ~65 | ~46 | 0.43 | ~1.5 |

`pairSync` is the total wall time of the serialized framebuffer-readback path per frame; the three
columns after it are its measured components (the rest is CPU command-list building).

## Diary-screen frame decomposition (~65 ms in fullSync, ~14.5 FPS)

| Component | ms/frame | share | what it is |
|---|---|---|---|
| **gpuWait** | ~46 | 71% | 20 serialized GPU submit+execute+fence waits, ~2.3 ms each |
| CPU command-building | ~17 | 26% | building 20 framebuffer pairs' command lists (pairSync − the measured parts) |
| uploadWait | ~1.5 | 2% | `waitForUploaders()` blocking on texture-uploader threads |
| cpuCopy | ~0.4 | <1% | `copyNativeToRAM` memcpy of rendered pairs back into RDRAM |

Sanity: 46 + 17 + 1.5 + 0.4 ≈ 65 ms ✓. The CPU-readback-memcpy theory is **dead** (0.4 ms). The
cost is GPU-fence serialization, not data copying.

## Root cause

With `copyWithGPU=false` (`src/main/rt64_render_context.cpp:356`, set because GPU copies "break
effects in menus" — the code's own TODO), rt64 forces one blocking submit+fence **per framebuffer
pair**: `fbPair.syncRequired = (f > 0)` at `lib/rt64/src/hle/rt64_state.cpp` overwrites the natural
value on every pair. The diary screen emits **20 pairs/frame** (10× gameplay's 2), so 20 serialized
fences ≈ the whole frame.

`syncRequired` is **not gratuitous** — it is a real read-after-write barrier (framebuffer-as-texture:
a later pair samples pixels an earlier pair wrote; set at `rt64_rdp.cpp:151` by address overlap, and
at `rt64_state.cpp:558` on format change). So "batch the pairs into one submit" would serve stale
data — a correctness break, not just a visual one.

The blanket override and the copies-off short-circuit at `rt64_rdp.cpp:139` are a **matched design
pair**, not two independent oddities: with copies off, rt64 deliberately skips dependency detection
entirely (the overlap detector early-returns) and compensates by fencing *every* pair. The override
is therefore **load-bearing for correctness**, not conservative paranoia — that is the "why blanket?"
answer the squashed history couldn't give us, recovered from source structure.

The counter's copies-off `naturalFences = 12` is **not** the count of genuine dependencies — with the
detector short-circuited it is format-changes only, an artifact. The screen's true serialized
dependency count is **~18** (see "The 12→18 anomaly" below), so of the 20 forced fences only ~2 are
actually gratuitous. This is exactly why a precise-barrier fix cannot help — detailed below.

## DVFS feedback loop (reasoned, **unmeasured on this device**)

The GPU sat pinned at its **minimum** clock (305 MHz of 670) throughout, thermal status 0 — not
throttling. The likely mechanism: 20 tiny gap-separated submits never sustain enough load for the
`msm-adreno-tz` governor to ramp, so each fence's GPU work runs at half clock, inflating the ~2.3 ms.
This is a **feedback loop** — serialization causes the low clock, the low clock slows each fence —
which cuts optimistically: any fix that reduces serialization should let the GPU ramp, shrinking the
per-fence cost *on top of* cutting the count. So the 46 ms gpuWait is not a fixed floor.

**This is reasoned, not measured.** The clean test — pin the GPU to max via
`/sys/class/kgsl/kgsl-3d0/` and re-measure — needs root, which this device does not have (`su`
absent, kgsl nodes root-only `rw`). **Open user-side ask**: the Retroid firmware may expose a
"performance mode" that raises the DVFS floor; dev-device can toggle it manually in a future session and
re-capture to bound how much of the 2.3 ms/fence is clock-inflated vs fixed sync overhead.

## Fix-option ledger

| # | Fix | Effect on fences | Est. result | Menu glitch? | Notes |
|---|---|---|---|---|---|
| **(a)** | Re-enable `copyWithGPU=true` (fix the upstream menu-effect breakage first) | tile copies satisfy deps on-GPU → fences drop to the A/B number | potentially large; **best upside** | **yes** (the reason it's off) | needs the glitch fixed upstream; see fork history `fb1dc22` "framebuffer fix for file select menus" |
| **(b)** | Precise barrier: keep copies off, fence only real deps | 20 → 12 | ~50 ms/frame → **~20–22 FPS** (8 fences × ~2.3 ms ≈ 18 ms off a ~69 ms frame → 3 vsyncs under FIFO) | **no** | improvement, **not a cure**; lowest risk |
| **(c)** | Reduce the 20-pair count itself | attacks the 10× multiplier | unknown, possibly largest structural win | n/a | needs RE: *why* 20 pairs on this screen, can they merge |

Honest read: **(b) alone is partial.** The serious combination is **(b) + (c)**, or **(a)** if the
glitch turns out cheap to fix. The A/B decides which.

## Pending: the `copyWithGPU=true` A/B

Unique value the counters can't give: (1) how many of the 12 genuine deps tile-copies satisfy
on-GPU — with copies on, the blanket override is skipped *and* tile copies relieve
`callTile.syncRequired` before it reaches the pair flag, so the copies-on `naturalFences` reading is
a **post-relief** dependency count; comparing it against 12 is a clean measurement of tile-copy
relief. (2) The **visual glitch severity** — product data that decides whether fix (a) is "ship the
tradeoff" or "real upstream work."

**Pre-registered predictions:**
- fences land somewhere in **[1, 12]**.
- Near **1** → tile copies relieve almost everything → fix (a) is a config flip gated purely on how
  bad the glitch looks.
- Near **12** → tile copies don't help this screen → the serious candidates are **(b) + (c)**.

### A/B result (captured 2026-07-19) — fix (a): cure REFUTED, mitigation RETAINED

Same diary screen, `copyWithGPU=true` (temporary flip, reverted after capture):

| Metric | copies OFF | copies ON |
|---|---|---|
| present FPS | 14.0 | 17.4 |
| pairs/frame | 20 | 20 |
| fences/frame | 20 | **19** |
| naturalFences/frame | 12 | **18** |
| pairSync ms | 65 | 56 |
| **gpuWait ms** | **46** | **46** |
| CPU-build remainder ms | ~17 | ~9 |

Pre-registered prediction was fences ∈ [1, 12]. **Actual: 19.** Tile copies do not relieve this
screen's dependencies. Conclusions:

1. **Fix (a) is dead as a *serialization cure*.** Enabling GPU copies moved fences 20→19 and `gpuWait`
   not at all. The ~46 ms serialized GPU-fence cost is **inherent to 20 framebuffer pairs**, not an
   artifact of the `copyWithGPU=false` forced-sync policy.
2. **But (a) is a real mitigation, NOT moot** (correction 2026-07-19 later session — the original
   "moot" reading below was wrong). The 14.0→17.4 FPS gain is **+24% on the worst screen, glitch-free**,
   entirely from **CPU-side** command-building savings (remainder ~17→~9 ms). "Doesn't fix the
   serialization" was mis-stated as "doesn't help perf." It helps perf; it just isn't the cure. See
   "What actually remains" lever 1 — copies-on is the cheapest evidenced smoothness win for this game.
3. **No visible glitch** on this screen (observed on device AND the desktop llvmpipe rig) — so the
   "breaks effects in menus" reason for disabling copies doesn't manifest on mnsg's file-select. The
   reason to keep validating is *other* screens/gameplay, never captured with copies on.
4. **Anomaly, unexplained:** `naturalFences` *rose* 12→18 with copies on. Tile copies should relieve
   deps (`!callTile.tileCopyUsed` at `rt64_state.cpp:1012`), not add them. Mechanism not verified —
   flagged for follow-up. Consequence: the A/B **conflates** "skip the blanket override" with
   "enable tile copies," so it is **not** a clean test of fix (b).

### The 12→18 anomaly, explained — and it kills fix (b)

Static trace (verified 2026-07-19) of what sets `fbPair.syncRequired`:

- `callTile.syncRequired` ← `checkTileCopyTMEM` (`rt64_state.cpp:345`) ← TMEM regions tagged by
  `insertRegionsTMEM` ← **only** `RDP::checkFramebufferOverlap` (`rt64_rdp.cpp:152`).
- `checkFramebufferOverlap` **early-returns when `copyWithGPU` is off** (`rt64_rdp.cpp:139`), before
  it tags anything.

So with copies **off**, the framebuffer-as-texture overlap detector is short-circuited:
`callTile.syncRequired` is always false, the `:1013` escalation never fires, and the only surviving
setter of `fbPair.syncRequired` is `:558` (color/depth **format change**).

**Consequences:**
- The copies-off `naturalFences = 12` is **format-changes only**. The real framebuffer-as-texture
  dependencies are *undetected* in that mode; the blanket `(f>0)` override is what covers them. The
  "8 gratuitous fences" reading was wrong — they guard real-but-undetected reads.
- The copies-on `naturalFences = 18` is the closer estimate of the screen's true serialized
  dependency count (format changes + detected overlaps not relieved by a tile copy).
- **Naive fix (b)** — copies off + skip the blanket override — is a **correctness bug**: the ~6
  undetected reads go unfenced → next pair samples stale RDRAM → menu corruption.
- **Correct fix (b)** — move the `rdp:139` early return so detection runs without GPU copies, then
  fence precisely — lands at ~18–19 fences (≈ copies-on), and likely *worse* FPS than copies-on
  because it forgoes the tile-copy CPU savings (~17 ms vs ~9 ms remainder). **Not worth building.**

**Revised ledger:**
- (a) copies-on — **crossed off as a serialization cure** (fences 20→19, `gpuWait` unchanged), but
  **retained as a mitigation**: it is +24% FPS (14.0→17.4) glitch-free on this screen. See "What
  actually remains" lever 1; it needs a copies-on validation pass on gameplay/other screens, not
  engine work. (Correction 2026-07-19: an earlier revision wrote this off entirely.)
- (b) precise barrier — **crossed off**: the true dependency count is ~18, so precise fencing can't
  beat copies-on's ~17 FPS, and the cheap version is incorrect. (Fully verified via the anomaly
  trace above — the counter and the A/B together did their job.)
- (c) **reduce the pair/dependency count** — the leading structural lever. The ~46 ms `gpuWait` is
  ~18 genuine serialized framebuffer-as-texture deps at min clock; only cutting the count helps.
  Entry point: the desktop RT64 inspector already visualizes per-pair `syncRequired`. Needs RE into
  why this screen composites via ~20 framebuffer pairs and whether they can be merged.
- **DVFS / clock** — co-leading, config-independent: the ~46 ms is at 305 MHz (min). Note **(b)/(c)
  differ here**: (b)'s 18 tiny serialized submits still won't let the governor ramp, so (b) gets no
  DVFS bonus; (c) — fewer, fatter submits — is the only fix that could *also* raise the clock, so its
  upside compounds. Unmeasurable on this device (no root); the Retroid firmware performance-mode
  toggle is the only lever, and is a pending user-side capture.

## Desktop-inspector on-ramp (for the option-(c) RE)

The desktop RT64 inspector visualizes per-pair `syncRequired` and runs off-device — the cheap entry
point for the (c) investigation (why ~20 pairs, can they merge). `developerMode` is wired to `debug`
(`src/main/rt64_render_context.cpp:346`); F1 opens the inspector.

WSL build prerequisites (this environment had none of these; `sudo` is available):

```
sudo apt-get install -y libsdl2-dev libfreetype-dev libvulkan-dev libgtk-3-dev
cmake -B build-desktop -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build-desktop --target Goemon64Recompiled -j"$(nproc)"
./build-desktop/Goemon64Recompiled            # F1 = RT64 inspector
```

`libgtk-3-dev` is a second-order dep (nativefiledialog-extended), not obvious from the top-level
CMake.

**RESOLVED (2026-07-19, later session): the desktop build now builds and launches.** Both blockers
below were fixed; the binary runs under WSLg and the Vulkan/WSLg risk is retired. Turnkey configure is
now just:

```
cmake -B build-desktop -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-desktop --target Goemon64Recompiled -j"$(nproc)"
```

The `clang` selection is **load-bearing, not stylistic** — see blocker 2. Verified end to end:
configure 0, build `[848/848]` 0, no `--allow-multiple-definition`, no `-DFILE_TO_C` flag.

1. **`FILE_TO_C` unset on the desktop branch — FIXED in `CMakeLists.txt`.** The desktop `else()`
   branch now sets `set(FILE_TO_C file_to_c)` / `set(FILE_TO_C_DEP file_to_c)` (the in-tree native
   target), mirroring rt64's own non-Android setting (`lib/rt64/CMakeLists.txt:85`) and the Android
   branch. Root cause: rt64 sets `FILE_TO_C` in *its* directory scope, but the app-level
   `build_vertex_shader`/`build_pixel_shader` calls run in the app scope where it was unset → empty
   COMMAND → the step tried to exec the `.spv` (`Permission denied`, exit 126). The old
   `-DFILE_TO_C=…build-host-tools/file_to_c` workaround is no longer needed.
2. **Multiple-definition at final link — was a COMPILER problem, not a missing patch-exclusion.**
   The "Android build applies a patch-exclusion or weak-symbol mechanism the desktop path lacks"
   framing was wrong. There is no patch-exclusion step: `func_8000F6E8_102E8` et al. really are
   defined in *both* `RecompiledFuncs/funcs_*.c` and `RecompiledPatches/patches.c`. What differs is
   `RECOMP_FUNC` (`lib/N64ModernRuntime/N64Recomp/include/recomp.h`): under **clang** it expands to
   `extern inline __attribute__((weak,noinline))` → **weak** symbols, so duplicate defs link fine and
   the patch interposes; under **GCC** it expands to `__attribute__((noipa,…))` → **strong** symbols
   → `ld.bfd` multiple-definition error. The failed smoke-test had configured with `/usr/bin/cc`
   (GCC). Building with clang (as the NDK does, and as the whole recomp ecosystem expects on Linux)
   makes the link clean with zero CMake changes. No `--allow-multiple-definition` — that hack is
   unnecessary and correctly avoided.

**Launch / Vulkan-under-WSLg — RETIRED.** `./build-desktop/Goemon64Recompiled` initializes Vulkan
(plume: loader 1.4.341, requesting 1.2), opens an SDL/x11 window, and runs the render loop. **Caveat:
it selects `llvmpipe` (Mesa software rasterizer), not the WSLg GPU passthrough** — vendor `0x10005`,
device "llvmpipe (LLVM 21.1.8)". For the option-(c) RE this is acceptable: the inspector visualizes
the per-pair framebuffer *dependency structure* (why ~20 pairs, can they merge), which is deterministic
and GPU-independent. It only means reaching the Select Adventure Diary screen renders slowly, and that
absolute frame timings on desktop are not comparable to the device.

Net: the on-ramp is done. Remaining work is the **interactive inspector RE itself** — launch, pick the
ROM, load a save, navigate to the diary screen, press **F1**, and read the per-pair `syncRequired`.
That must be driven in a real desktop session by the user (it needs GUI interaction, and llvmpipe is
slow to reach the screen).

## Option-(c) RE: the 20 pairs decoded (desktop, 2026-07-19)

With the desktop build working (on-ramp above), the `[g64prof]` instrumentation was forced on for
desktop (`-DRT64_PROFILE_LOGCAT=1`) and extended with a **per-pair enumeration** (address / fmt / siz
/ width / depth / draw-call count / natural-dep / flushReason), added in `rt64_state.cpp` inside the
existing `RT64_PROFILE_LOGCAT` block. Raw capture: `docs/re-notes/fixtures/menu-framerate-pair-enum.txt`.

**The desktop build reproduces the device's structural counts exactly** (llvmpipe software Vulkan, so
timings differ but counts are GPU-independent): diary screen `pairs=20, fences=20, naturalFences=12`;
gameplay `pairs=2`. This is the local, iterable rig the investigation was blocked on.

**What the 20 pairs are** — one repeating structure per frame:

- `pair[0]`  main clear (color==depth==`002D1800`, w320, `fillRect`, 1 call)
- `pair[1]`  main setup (`00286800`, w320, 2 calls, `SampleColor`)
- `pair[2..18]` a **3-pair cycle repeated ~6×**: `[001E46F0 w=8, 2 calls, natural]` →
  `[001E46F0 w=256, 2 calls, natural]` → `[main w=320, 4 calls, ColorChanged]`
- `pair[19]` the **actual diary UI** — `00286800`, w320, **144 draw calls**, `DLEnd`

Main color target double-buffers `00261000`↔`00286800`; depth constant `002D1800`; scratch is
`001E46F0` (rendered at w=8 then w=256 each cycle). flushReason histogram over the 20: **17
`ColorChanged`, 2 `SampleColor`, 1 `DLEnd`**.

**Findings:**

1. The pair explosion is **the animated color-cycling striped background** (the green/orange waves).
   It renders a tiny scratch buffer and composites into main, **ping-ponging the render target ~6×**.
   Those target switches are the 17 `ColorChanged` flushes — the entire delta over gameplay's 2 pairs.
2. **19 of 20 pairs are near-empty** (1–4 draw calls). The real UI is the single 144-call `pair[19]`.
   So ~95% of the serialized fences are spent on a background effect that costs almost nothing to draw.
3. The cost is **serialization, not work**: `gpuWait` dominates, `cpuCopy≈0.3ms` is negligible. 20
   blocking submit+fence round-trips for a background that's mostly 2-call passes.
4. The 12 natural deps are exactly the `001E46F0` scratch renders (sampled by the composite → genuine
   read-after-write). The other 8 pairs are `ColorChanged`-only, force-fenced solely by `copyWithGPU=false`.

**This reframes option (c):** the lever is the background effect's target ping-ponging, not generic
framebuffer merging.

### copies-ON A/B on the rig — fence-policy is now MEASURED dead (not estimated)

`copyWithGPU` was made env-selectable (`G64_COPY_GPU=1`, `rt64_render_context.cpp`) and the diary
screen re-captured. Direct comparison on the same rig:

| metric | copies OFF | copies ON |
| --- | --- | --- |
| pairs/frame | 20 | 20 |
| fences/frame | 20 | **19** |
| naturalFences/frame | 12 | **18** |
| gpuWait (llvmpipe) | ~59ms | ~92ms |

- **The true dependency count is 18, measured** — with `checkFramebufferOverlap` running (copies on,
  no early-return at `rt64_rdp.cpp:139`), the 6 composite pairs flip `natural=0 → natural=1`; they are
  genuine framebuffer-as-texture reads of the `001E46F0` scratch. This confirms the earlier static
  "~18 / detector-artifact" estimate *exactly*, and confirms the note above's caution: a precise
  barrier that fenced only the copies-off natural=12 would drop 6 real dependencies → corruption.
- **Fence-policy (option 1) is dead**: 18 of 20 pairs are real read-after-write deps, so the best a
  correct precise barrier can do is 20→18 (~10%). Not a smoothness fix.
- **copies-ON reproduces the device's structural counts locally** (fences 20→19, naturalFences 12→18).
  It is **slower on llvmpipe** (gpuWait ~59→~92ms) — but that is a **software-rasterizer artifact**:
  llvmpipe emulates the GPU tile copies on the CPU, so copies-on *adds* CPU work here. **This says
  nothing about the device.** The authoritative device A/B (§"A/B result") shows the opposite:
  **14.0→17.4 FPS, glitch-free** — a real +24% from CPU-side command-building savings (~17→~9ms).
  Do NOT read the llvmpipe slowdown as "copies-on is bad"; it is a rig limitation.
- **copies-ON did NOT visibly break the diary background** on llvmpipe (user-observed), matching the
  device A/B's "no visible glitch on this screen." The breakage the `rt64_render_context.cpp:353`
  comment cites is therefore likely device/driver-specific or specific to the other game (its TODO
  names Goemon's Great Adventure underwater), not mnsg's file-select.

### Why tile copies decline the scratch reads — the cheaper suspect (step ii, 2026-07-19)

Before scoping a submit-batching rework, trace why rt64's *existing* GPU-side dependency mechanism
(tile copies) declines the scratch reads: copies-on `naturalFences=18` means 6 reads had
`syncRequired && !tileCopyUsed`, i.e. `makeFramebufferTile` was asked and **declined**. Instrumented
every exit of `makeFramebufferTile` (`rt64_framebuffer_manager.cpp`, `[g64tile]`, behind
`RT64_PROFILE_LOGCAT`) and captured the diary screen with copies on. Result — **one reason, one
buffer**:

```
15276  ACCEPT                fb=001E46F0..001E86F0   (scratch, MOST reads get a tile copy)
 5092  DECL:loadblk-misalign fb=001E46F0..001E86F0   (a SUBSET decline)
 6792  ACCEPT                fb=002AC000..002D1800
 6792  ACCEPT                fb=00261000..00286800
 6784  ACCEPT                fb=00286800..002AC000
```

Geometry (scratch `w=256, siz=2` → 512-byte row stride; loadBlock = `lineW=0, tileH=0`):

| loadBlock read | offset | in stride terms | result |
| --- | --- | --- | --- |
| `001E46F0` | 0 | row 0, aligned | ACCEPT |
| `001E66F0` | 0x2000 | row 16, aligned | ACCEPT |
| `001E76F0` | 0x3000 | row 24, aligned | ACCEPT |
| **`001E4E70`** | **0x780** | **row 3 + 384 B (pixel 192)** | **DECL:loadblk-misalign** |

The scratch is loaded as **four linear `loadBlock` chunks**; three land on a 512-byte row boundary and
get GPU tile copies, the fourth starts **mid-row** and spans ~3.75 rows — a linear run tracing a
**staircase** across the 2D target, which a rectangular tile copy can't represent, so
`makeFramebufferTile` correctly bails at `:478` (`fromLoadBlock && multipleRows && misalignedRow`) and
it falls back to a CPU fence. This is a **1D-loadBlock-vs-2D-rectangular-tile impedance mismatch**, not
a correctness bug.

**Why this matters more than "6 fewer fences":** with copies on, `renderAndSynchronize` (the per-pair
`execute()/wait()`) only fires for pairs whose `syncRequired` is set, and a dependency satisfied by a
tile copy does **not** set it (`rt64_state.cpp:1012`). So improving tile-copy *coverage* removes the
per-pair sync *trigger* — if tile copies covered all deps, most pairs would fold into the single
end-of-frame sync and the 20 serial round-trips collapse toward one, **inside the current
architecture** (no submit-loop rewrite). The fix shape is a **linear/1D framebuffer-tile-copy path**
for loadBlock reads.

**Step iii (upstream check, 2026-07-19):** canonical upstream RT64 (`rt64/rt64` main)
`makeFramebufferTile` has the **identical decline, no linear path** — so this is **net-new engine
code, not a rebase/cherry-pick**.

**Open questions before this is "the fix" (do not overclaim 60 FPS):**
1. Fixes the **6 loadBlock deps** only. The other **12 fences are format-changes** (`rt64_state.cpp:558`),
   a separate mechanism — unknown whether those can also be satisfied on-GPU. If not, ~12+ fences remain.
2. Whether fewer fences → proportionally faster is the **latency-vs-compute question** (step iv,
   device-only; llvmpipe can't answer it). If fixed per-fence latency dominates, 12 fences ≈ 28ms.
3. Only helps with **copies-on** (with copies off, `makeFramebufferTile` is never called) — rides on
   the copies-on validation pass.

### What actually remains for smoothness

The 20 passes are a genuine serial dependency chain (render tiny scratch → composite samples it →
repeat ~6×). **Fence policy** can't move it (18 of 20 are real deps). **Copy mode** does not move the
~46ms serialization either — but it *does* move FPS +24% via CPU-side savings, so it is a real
mitigation, not nothing. Levers, best-evidenced first:

1. **Ship `copyWithGPU=true` for this game (mitigation, cheapest, MEASURED win).** The device A/B is
   **14.0→17.4 FPS glitch-free** on the worst screen — the only lever on this list with a measured,
   evidenced gain already attached. It does **not** cure the serialization (gpuWait stays ~46ms); the
   win is command-building (~17→~9ms). Why it's currently off doesn't apply here: the
   `rt64_render_context.cpp:353` TODO cites *Goemon's Great Adventure* underwater (the **other** game)
   and even proposes per-game configs as the fix; no glitch was seen on mnsg's diary screen on either
   rig. **Validation is a test pass, not engine work.** DONE (desktop rig, `G64_COPY_GPU=1`,
   2026-07-19): a **visual sweep across gameplay + menus/effect screens was clean** (no glitches), and
   the first **gameplay copies-on structural capture** shows gameplay is **dependency-free**
   (`pairs=2, fences=1, naturalFences=0`) — so copies-on adds nothing to the common case and cannot
   hurt the 95% of playtime that is gameplay. STILL PENDING: the same sweep **on the device** to catch
   Adreno-specific breakage the llvmpipe rig can't (the historical breakage was driver/other-game
   specific), plus device timing. If the device sweep is clean, ship copies-on for mnsg via a per-game
   `copyWithGPU` config (what the `rt64_render_context.cpp:353` TODO already proposes) and retire the
   `G64_COPY_GPU` env toggle into it. Compounds with lever 2.
2. **DVFS / GPU clock (pending, zero-build, user-side).** The device measured `pairSync≈46ms` at a
   **DVFS-floored 305 MHz** — the light menu workload never triggers a clock boost, so the serialized
   fences run at minimum clock. A firmware performance-mode capture tests how much of the stall is
   clock-inflation vs genuine work. Best value-per-effort of the *cure*-side options; run it together
   with copies-on for the true best-available configuration.
3. **Improve tile-copy coverage: a linear/1D loadBlock path (targeted, the promising cure-candidate).**
   See "Why tile copies decline the scratch reads" above. The per-pair `execute()/wait()` fires only
   for pairs with `syncRequired` set, and a tile-copied dependency doesn't set it — so making the
   declined loadBlock reads succeed on-GPU could fold most pairs into one end-of-frame sync *within the
   current architecture*. Net-new (upstream RT64 lacks it too), but far smaller than a submit-loop
   rewrite. Gated by the three open questions above (format-change fences, latency-vs-compute, requires
   copies-on).
4. **RT64 engine: rework the submit loop to pipeline fbPairs on-GPU** (large — the fallback cure). Only
   if lever 3 can't reach all the deps (notably the 12 format-change fences). RT64 does a blocking
   submit+fence per framebuffer pair in `renderAndSynchronize(f)` regardless of copy mode; coalescing
   into one submission with intra-frame barriers is the general fix but reworks the fbPair loop.
   **Consistency constraint:** RDRAM must stay *eventually* consistent (game CPU reads framebuffer
   memory between frames), so the shape is barriers/tile-copies intra-frame, **one** fence + **one**
   RDRAM copy-back at frame end — "coalesce" must never become "skip the copies".
5. **Game-side patch to the effect** (risky, changes visuals) — collapse the 6 serial composite cycles.
   Not recommended; the effect is a deliberate look.

## Out of scope but open

- **Gameplay is locked to 30 FPS**, and this data **exonerates the fence path for it**:
  pairSync ≈ 4.7 ms of a 33 ms frame, so whatever caps gameplay at 30 (the earlier "vsync-divisor
  pacing" guess) is a **separate investigation with its own missing evidence**. Do not conflate it
  with the menu stall.
