# Android profiling results — menu framerate

Companion to [`android-profiling-plan.md`](android-profiling-plan.md). This doc records the first
real capture: the **Select Adventure Diary** screen runs at ~14 FPS on the test device, and we now
know why, in measured detail.

Status: **diagnosis confirmed and decomposed; fixes (a) and (b) both crossed off by evidence.** The
`copyWithGPU=true` A/B refuted (a); a static trace of the 12→18 anomaly refuted (b) — the screen has
~18 genuine serialized framebuffer-as-texture dependencies at min GPU clock, so no fence-policy
change helps. **Only (c) reduce the pair/dependency count, and DVFS/clock, remain.** Both need work
outside this session (RE via the desktop inspector; a firmware performance-mode capture).

## Provenance

- **Device**: Retroid Pocket 5 (Snapdragon 865 / Adreno 650), 1080×1920 @ 60 Hz, **not rooted**.
- **Numbers below** come from the `[g64prof]` instrumentation on rt64 branch `diag/menu-framerate`,
  commit `6c07782` (local only, not pushed; deliberately kept off `goemon-android` — it is
  diagnostic scaffolding, remove-or-gate before release). Build: local `gradlew assembleDebug`,
  arm64-v8a debug, `copyWithGPU=false` (shipping default) unless stated.
- **Capture**: single session 2026-07-19, logcat `-s Goemon64-stdio`, steady-state samples only
  (first ~2 s after a scene change discarded while the profiler rings warm up).
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

### A/B result (captured 2026-07-19) — fix (a) REFUTED

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

1. **Fix (a) is dead as a performance fix.** Enabling GPU copies moved fences 20→19 and `gpuWait`
   not at all. The ~46 ms serialized GPU-fence cost is **inherent to 20 framebuffer pairs**, not an
   artifact of the `copyWithGPU=false` forced-sync policy.
2. The modest 14→17 FPS gain came entirely from **CPU-side** work (remainder ~17→~9 ms), not fences.
3. **No visible glitch** on this screen (observed) — so the "breaks effects in menus" reason for
   disabling copies doesn't manifest here. Moot, since (a) doesn't help perf anyway.
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
- (a) copies-on — **crossed off**, refuted by the A/B (fences 20→19, `gpuWait` unchanged).
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

**Smoke-test result (2026-07-19): the desktop build does NOT currently build in this repo — two
distinct blockers, so budget real porting work before the inspector RE, not just an apt install.**
We never reached the Vulkan-init / WSLg-GPU question; the build fails earlier.

1. **`FILE_TO_C` unset on the desktop branch.** `CMakeLists.txt:485` sets `FILE_TO_C` (host shader
   embedder) only in the Android branch; the desktop `else()` at `:487` sets `DXC`/`SPIRVCROSS` but
   never `FILE_TO_C`. So `build_vertex_shader`/`build_pixel_shader` emit an empty command and the
   shader step tries to *execute the `.spv`* (`Permission denied`, exit 126). **Workaround that
   worked:** pass `-DFILE_TO_C="$PWD/build-host-tools/file_to_c"` (reuse the Android pipeline's host
   tool). Proper fix: have the desktop branch set `FILE_TO_C` to the native `file_to_c` target (as
   the NRM/patches rules already do with bare `file_to_c`).
2. **Multiple-definition at final link.** Every `RECOMP_PATCH`'d function is defined in *both*
   `RecompiledPatches/patches.c` and the original `RecompiledFuncs/funcs_*.c`; `ld.bfd` rejects the
   duplicates (`func_8000F6E8_102E8` et al.). The Android/gradle build applies a patch-exclusion or
   weak-symbol mechanism the desktop CMake path does not. **Not worked around** — resolving it
   properly (understand how the Android build excludes patched originals, replicate on desktop; a
   blanket `--allow-multiple-definition` would let the patch win by link order but risks a subtly
   wrong binary, unacceptable for an inspector session). This is the first task for next session.

Net: the desktop-inspector on-ramp is a **porting task**, not a turnkey build. Reaching a launchable
binary is next session's opening move; the Vulkan-under-WSLg risk is still unretired behind it.

## Out of scope but open

- **Gameplay is locked to 30 FPS**, and this data **exonerates the fence path for it**:
  pairSync ≈ 4.7 ms of a 33 ms frame, so whatever caps gameplay at 30 (the earlier "vsync-divisor
  pacing" guess) is a **separate investigation with its own missing evidence**. Do not conflate it
  with the menu stall.
