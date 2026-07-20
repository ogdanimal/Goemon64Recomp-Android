# Menu-framerate investigation — adversarial-review handover

Self-contained synthesis for a hostile reviewer. Every claim is tagged **[OBS]** (observed/measured),
**[INF]** (inference from observations), or **[HELD]** (asserted but not yet validated — attack these
first). Authoritative detail: [`docs/android-profiling-results.md`](../android-profiling-results.md).
Evidence corpus: [`fixtures/menu-framerate-pair-enum.txt`](fixtures/menu-framerate-pair-enum.txt).

Date of this handover: 2026-07-19.

---

## 0. One-line status

The Select Adventure Diary screen runs ~14 FPS (gameplay 30) on a Retroid Pocket 5. The mechanism is
decomposed and two fence-side fixes are refuted. **RESOLVED 2026-07-19 device session (this closes the
two [HELD] items below):** the root cause is **clock-bound per-renderpass GPU compute** — `GPU_hw ∝
1/clock`, decoupled from N64 draw count — and **DVFS is the dominant lever**: the device perf profile
(305/400→587 MHz) **nearly doubled FPS (12–15→26)**. The default-mode `msm-adreno-tz` governor parks
the clock at 305–400 MHz because GPU busy holds at ~68 %. This **refutes** the earlier lean toward
"DVFS is out / fixed submit-latency." Full evidence + numbers:
[`fixtures/menu-framerate-device-fence-timing.txt`](fixtures/menu-framerate-device-fence-timing.txt).

## 1. Two measurement environments — never conflate (this is the #1 review trap)

- **DEVICE** — Retroid Pocket 5, Snapdragon 865 / Adreno 650, not rooted, arm64 debug APK.
  **Authoritative for all timing.**
- **DESKTOP RIG** — WSL `build-desktop`, clang, `-DRT64_PROFILE_LOGCAT=1`, **llvmpipe (software
  Vulkan)**. Authoritative for **structural counts** (pairs / fences / naturalFences / flushReason)
  and **logic decisions** (tile-copy accept/decline). **All rig *timing* is invalid** — llvmpipe
  emulates GPU work on the CPU, so e.g. copies-on looks *slower* on the rig and *faster* on device.
  Any argument that leans on a rig millisecond number is wrong by construction.

## 2. Claim ledger

### Observed [OBS]
1. Diary screen: **20 framebuffer pairs/frame**; gameplay: **2**. (device + rig agree)
2. Device copyWithGPU=false: fences=20, gpuWait≈46ms ≈ whole frame; cpuCopy≈0.3ms (negligible).
3. Device copies-on A/B (temporary flip): **14.0→17.4 FPS (+24%)**, gpuWait **46→46** (unchanged),
   CPU-build remainder **~17→~9ms**, fences **20→19**, naturalFences **12→18**, **no visible glitch**
   on this screen.
4. Rig reproduces the *counts* exactly: copies-off pairs=20/fences=20/naturalFences=12; copies-on
   pairs=20/fences=19/naturalFences=18. Gameplay: pairs=2, naturalFences=0.
5. Pair enumeration (rig): the 20 pairs = 1 clear + 1 setup + **a 3-pair cycle repeated ~6×**
   (`[scratch 001E46F0 w=8][scratch w=256][composite→main]`) + **one 144-call UI pair (pair 19)**.
   The other 19 pairs are 1–4 draw calls each. Main target double-buffers `00261000`↔`00286800`,
   depth constant `002D1800`.
6. Tile-copy decline (rig, copies-on): every decline is `DECL:loadblk-misalign`, **only** on the
   `001E46F0` scratch (15276 ACCEPT vs 5092 DECL on that fb; all other fbs always ACCEPT). Geometry:
   scratch `w=256, siz=2` → 512B stride; loadBlock chunks at offsets 0 / 0x2000 / 0x3000 align and
   ACCEPT, the chunk at **offset 0x780 (row 3 + 384B)** is mid-row, spans ~3.75 rows, and DECLINES.
7. Upstream RT64 (`rt64/rt64` main) `makeFramebufferTile` has the **identical** decline, no linear
   path. (WebFetch of the upstream source, 2026-07-19)
8. Repo `mnsg.z64` is the **decompressed build-time ROM** (32 MiB, XXH3=`0x380FC5D167CE89BA`), **not a
   runtime ROM**; runtime expects `0xDB1BC7EE0E6BEBA1` (the 16 MiB `mnsg.us.z64` from the device
   backup). Verified by recomputing XXH3 the way `check_hash` does.

### Inference [INF]
9. The 20 pairs are the animated color-cycling striped background ping-ponging the render target; the
   17 `ColorChanged` flush reasons are those target switches. (from #5)
10. The cost is **serialization**, not draw work: 19–20 pairs × per-pair `execute()/wait()`
    (`rt64_state.cpp:1461-1468`), each materializing a framebuffer back to RDRAM via `copyNativeToRAM`.
    (from #2, #5, code read)
11. Fixing the loadBlock decline (a linear/1D tile-copy path) would let the 6 scratch reads satisfy
    on-GPU; since a tile-copied dep doesn't set `syncRequired` (`rt64_state.cpp:1012`), that removes
    the per-pair sync *trigger*, potentially folding pairs into the end-of-frame sync **within the
    current architecture**. (from #6, code read) — **candidate, not proven to reach 60 FPS.**

### Resolved 2026-07-19 (were [HELD] — now settled by device measurement)
12. **"DVFS/clock is not the bottleneck." — REFUTED. DVFS is the DOMINANT lever.** The device perf
    profile raised the clock 305/400→587 MHz and nearly doubled FPS (12–15→26). The earlier "perf mode
    makes no difference" report does not hold on this screen with this toggle. The manipulation check
    the old note demanded was done: `devfreq/cur_freq` confirmed the toggle raised the clock, and
    `gpu_busy_percentage` held at ~68 % at every clock — so in default mode the `msm-adreno-tz`
    utilization governor never crosses its ramp threshold and parks at 305–400 MHz, starving the
    clock-bound work.
13. **"The 46ms is fixed submit/fence latency (compute-independent)." — REFUTED.** Per-pair fence
    timing (rt64 diag `bba84ab`) settled it: cost is **clock-bound GPU compute, decoupled from draw
    count**. `GPU_hw ∝ 1/clock` (near-zero fixed term); the 144-draw pair costs ~1.4 ms while the one
    ~37 ms fence per frame is **always a 2-draw pair** — it is the frame's GPU compute surfacing at the
    single CPU/GPU sync catch-up point, not fixed latency and not draw-scaled. The old "2.3 ms/fence"
    was a misleading 46÷20 average (exactly the trap flagged in §4 #2). The compute is per-renderpass
    composite/pixel work across the 20 upscaled render targets.
14. **The authoritative doc now carries the corrected conclusion** (DVFS resolved, clock-inflated not a
    fixed floor) — see `android-profiling-results.md` "DVFS feedback loop (MEASURED and CONFIRMED)".

## 3. Refuted hypotheses (do not re-litigate; attack the refutations if you disagree)

- **(a) copies-on as a *cure*** — REFUTED: fences 20→19, gpuWait unchanged (#3). **But retained as a
  +24% mitigation** (CPU-side savings), pending a device visual sweep. Earlier drafts wrote it off
  entirely; that was an error, corrected.
- **(b) precise barrier** (fence only the copies-off natural 12) — REFUTED: naturalFences=12 is
  *format-changes only* because `checkFramebufferOverlap` early-returns when copies are off
  (`rt64_rdp.cpp:139`); the true dep count is **18** (measured copies-on, #3/#4). Fencing only 12
  drops 6 real deps → corruption. A correct precise barrier lands ~18 ≈ copies-on. Not worth it.

## 4. Attack surface (where this is weakest — enumerated so you don't have to hunt)

1. **~~The whole "latency-bound" story hangs on the unvalidated DVFS null (#12).~~ RESOLVED** — the
   "latency-bound" story was itself wrong. DVFS measured as the dominant lever (#12); the clock-reading
   check was done and refuted the null.
2. **~~Per-fence latency-vs-compute (#13) is unmeasured.~~ RESOLVED** — per-pair fence timing measured
   it: clock-bound compute, draw-count-decoupled. The 2.3ms/fence average was indeed misleading (one
   ~37ms fence + nineteen ~1–3ms fences), exactly as this item warned.
3. **The tile-copy fix (INF #11) addresses only 6 of ~18 deps.** The other **12 are format-change
   fences** (`rt64_state.cpp:558`). Whether they're a *separate* mechanism is itself open: they may
   share origin with the *same* background effect — the scratch is re-specified `w=8 → w=256` every
   cycle, and a SetColorImage re-spec is exactly what `:558` keys on. (Nuance: the enumeration shows
   the scratch's `fmt/siz` **constant** across those re-specs, so if `:558` fences them it's via the
   width/re-spec, not a true fmt change — needs confirming against what `:558` actually reads.) If
   they do share origin, the loadBlock fix and the format-change question are **not independent** and a
   full grasp of the scratch's lifecycle may resolve both; if not, the linear-loadBlock candidate caps
   at 6 of 18 as stated. Either way this is the **first RE question after the two [HELD]-closing
   captures** (see §5 lever 3).
4. **All rig timings are llvmpipe** (§1). Any number in ms from the rig is non-transferable.
5. **copies-on device validation is incomplete** — visual sweep is clean on the *rig* across
   gameplay+menus, but Adreno-specific breakage is untested; only the diary screen has device timing.
6. **Enumeration is "one representative frame"** printed once/sec, not a full-frame-set audit; the
   pattern was stable across samples but the sampler could miss an occasional variant.
7. **Every device number is from a `assembleDebug` build** — the 14 FPS, the 46ms, the +24% delta.
   Relative conclusions almost certainly survive, but *absolute* figures may shift in a release build;
   no release-build capture exists.

## 5. Remaining levers (best-evidenced first)

1. **Ship copyWithGPU=true for mnsg** — +24% measured mitigation, glitch-free on this screen; needs a
   device visual sweep on gameplay/other screens. Not a cure (gpuWait unchanged).
0. **Device-class default graphics — SHIPPED 2026-07-19 (dev `fb59148` MSAA-off, `fe33491` 4x cap).**
   Android now defaults **4x + MSAA off** (was Auto + MSAA2X — the heavy config behind the ~14 FPS).
   Measured: 8x→30 FPS, 4x→52 FPS at fixed clock; MSAA off ≈ another ~20 %. Cheapest, highest-certainty
   lever; likely playable in *default* power mode. Non-destructive. Un-run check: device-verify
   default-mode FPS at 4x/MSAA-off with the perf profile OFF.
2. **DVFS / GPU clock** — **MEASURED as the single biggest lever (~2×), #12 resolved — but NOT
   app-forcible on this device.** Perf profile 305/400→587 MHz took the screen 12–15→26 FPS. Default
   governor parks at 305–400 MHz (busy ~68 % < ramp threshold). **ADPF is out here:** its GPU-duration
   path is API 34+, the Retroid is API 33, so app-side ADPF boosts CPU only, not the Adreno clock; no
   non-root GPU overclock on API 33. The clock rises only via the user's firmware perf profile or
   indirectly by raising utilization (lever 3/4). App-controlled path to the clock's benefit = cut GPU
   work (lever 0, shipped) + cut the pair count (lever 3/4).
3. **Linear/1D loadBlock tile-copy path** — targeted cure-*candidate* (INF #11); net-new (upstream
   lacks it, #7); gated by attack-surface #3. **Sequencing:** after the two [HELD]-closing captures,
   the first RE question is the scratch buffer's full lifecycle — whether the 12 format-change fences
   (§4 #3) share its origin (`w=8 → w=256` re-spec each cycle). If they do, one understanding resolves
   both the loadBlock decline and the format-change fences; if not, this lever caps at 6 of 18.
4. **Submit-loop rework** (large fallback cure). Consistency constraint: RDRAM must stay *eventually*
   consistent (game CPU reads FB memory between frames), so the shape is barriers/tile-copies
   intra-frame, **one** fence + **one** RDRAM copy-back at frame end — never "skip the copies".
5. **Game-side effect patch** — not recommended (changes a deliberate visual).

## 6. Reproduction

**Desktop rig build (turnkey):**
```
cmake -B build-desktop -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ [-DRT64_PROFILE_LOGCAT=1]
cmake --build build-desktop --target Goemon64Recompiled -j"$(nproc)"
```
`clang` is load-bearing: it makes `RECOMP_FUNC` weak (`recomp.h`), resolving patch/original duplicate
symbols; GCC makes them strong → multiple-definition link error. `FILE_TO_C` fix is on the desktop
CMake branch (dev `c4d896b`).

**Run / capture (rig):** first-run ROM must be the *runtime* ROM (`mnsg.us.z64`, hash `0xDB1B…`),
staged into `~/.config/Goemon64Recompiled/`. Set `developer_mode:true` in `graphics.json` for the F1
inspector. `G64_COPY_GPU=1` forces copies on. `g64prof`/`g64tile` lines print to stderr.

**Device APK (has g64prof+enum+per-pair fence timing, copies-off):**
`android/app/build/outputs/apk/debug/app-debug.apk` — REBUILT 2026-07-19 23:04 from the tree with rt64
on `diag/menu-framerate` @ `bba84ab`. Now emits a `sync=NN.NNms` column per pair in the enumeration.
Capture: `adb logcat -s Goemon64-stdio | grep -E 'g64prof|g64tile'`. On the Diary screen read the
`sync=` column against `calls=`: **flat sync across wildly different call counts ⇒ fixed submit/fence
latency (46ms compute-independent, [HELD] #13 → confirmed); sync that tracks calls ⇒ GPU compute.**

**Provenance (all pushed to `ogdanimal/rt64` `diag/menu-framerate`):**
- `6c07782` per-second g64prof · `7e49e1f` per-pair enumeration · `fa91fd6` tile accept/decline trace ·
  `bba84ab` **per-pair fence timing** (adds a `sync=NN.NNms` column to the enumeration — the decisive
  per-fence measurement for [HELD] #13; see §7). Verified in the arm64 object: new format strings baked
  in, old ones gone.
- Parent gitlink stays `8c73b3f` (goemon-android release line); diag is checked out locally so CI
  builds the release line. Instrumentation is behind `RT64_PROFILE_LOGCAT` — remove/gate before release.
- Main repo (branch `dev`, local, unpushed): `c4d896b` FILE_TO_C fix · `a82e44d` G64_COPY_GPU toggle ·
  docs `e8d1879`/`aaf460a` (+ this file).

## 7. Data collected 2026-07-19 device session (both [HELD] items CLOSED)

Done. Both captures were taken in one session (rt64 `bba84ab`, per-pair fence timing).
Evidence: [`fixtures/menu-framerate-device-fence-timing.txt`](fixtures/menu-framerate-device-fence-timing.txt).

1. **(i) sysfs GPU clock + busy, normal vs perf mode — DONE.** normal-mode governor parks at
   305–400 MHz (busy ~68 %); perf profile → 587 MHz; FPS 12–15 → 26. DVFS is the dominant lever.
2. **(iv) per-pair fence timing — DONE.** Every frame = one ~37 ms fence (always a 2-draw pair) +
   nineteen ~1–3 ms fences; the 144-draw pair is ~1.4 ms. `GPU_hw ∝ 1/clock`. Clock-bound compute,
   draw-decoupled.

**Done this session:**
- **Shipped device-class default graphics** (§5 lever 0): Android defaults 4x + MSAA off — dev
  `fb59148` (MSAA off) + `fe33491` (4x cap). Built against clean rt64 (`goemon-android`, gitlink
  `8c73b3f`); instrumentation reverted. Shippable APK at
  `android/app/build/outputs/apk/debug/app-debug.apk`.
- **ADPF ruled out** for app-side GPU clock on this API-33 device (§5 lever 2).

**Next:**
- **Device-verify** default-mode FPS at 4x/MSAA-off with the perf profile OFF → confirms the shipped
  fix makes it playable without perf mode (can't fresh-install-verify on dev-device's device — config
  already has MSAA off manually).
- **RE the scratch lifecycle** (§4 #3) toward cutting the pair count (lever 3/4), which also raises
  utilization and lets the governor ramp on its own — the durable route to retire "perf mode essential".
- **Device copies-on visual sweep** on gameplay/other menus (still the one un-run validation; rig sweep
  was clean but Adreno-specific breakage is untested). Needs a device build with copies-on hardcoded.
