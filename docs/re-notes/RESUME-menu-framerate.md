# Resume prompt — Android menu-framerate investigation

Paste the block below as the first message of a fresh session. **Single entry point is the handover
doc** ([`menu-framerate-handover.md`](menu-framerate-handover.md)); it leads with its own weaknesses
and tags every claim OBS/INF/HELD. Authoritative detail:
[`../android-profiling-results.md`](../android-profiling-results.md). Evidence corpus:
[`fixtures/menu-framerate-pair-enum.txt`](fixtures/menu-framerate-pair-enum.txt).

---

```
Resume the Goemon64Recomp Android menu-framerate investigation.

READ FIRST (single entry point, structured for adversarial review, claims tagged
OBS/INF/HELD): docs/re-notes/menu-framerate-handover.md. Authoritative detail:
docs/android-profiling-results.md. Evidence: docs/re-notes/fixtures/menu-framerate-pair-enum.txt.
Cite these; do NOT re-derive.

STATUS: mechanism fully decomposed; two fence-side fixes refuted by evidence; the
root-cause CATEGORY (fixed submit/fence latency vs GPU compute) is NOT yet proven
— gated on ONE device session.

SETTLED — do not re-litigate (handover §2-3):
- Diary screen = 20 framebuffer pairs/frame (gameplay 2); device gpuWait ~46ms ≈
  whole frame at copyWithGPU=false (one blocking submit+fence per pair).
- The 20 pairs = a ~6x-repeated color-cycling BACKGROUND effect ping-ponging the
  render target (scratch 001E46F0 -> composite to main), + ONE 144-call UI pair
  (pair 19). 19 of 20 pairs are near-empty (1-4 draws).
- Fence policy REFUTED: 18 of 20 are genuine deps (measured, copies-on).
- copies-on REFUTED as a serialization CURE (fences 20->19, gpuWait flat) BUT
  RETAINED as a +24% MITIGATION (device 14.0->17.4 FPS, glitch-free on this
  screen, from CPU command-build savings). Earlier "written off entirely" was an
  error, corrected.
- Tile-copy decline traced (step ii): the 6 escalated fences are
  makeFramebufferTile DECL:loadblk-misalign on the 001E46F0 scratch — a mid-row
  loadBlock chunk (offset 0x780) can't be a rectangular tile copy; 3 of 4 chunks
  accept. Upstream RT64 (rt64/rt64) has the IDENTICAL decline -> a linear/1D
  loadBlock copy path is NET-NEW (candidate cure, handover §5 lever 3).

TWO [HELD] / UNVALIDATED — attack first (handover §2 #12-14, §4):
- DVFS null: "perf mode makes no difference" is NOT valid evidence until a
  clock-reading manipulation check confirms the toggle raised the GPU clock. If
  the clock stayed floored (~305MHz), the null means the toggle didn't fire.
  "inherent 46ms" is deliberately kept OUT of the authoritative doc until this
  resolves.
- per-fence latency-vs-compute UNMEASURED (2.3ms/fence is 46ms/20, an average).

NEXT — ONE device session closes both [HELD] (handover §7):
1. Investigating side: add per-pair fence timing to the enumeration (rt64 diag),
   rebuild the arm64 debug APK.
2. Device (dev handheld): capture sysfs GPU clock in normal vs performance mode
   (cat /sys/class/kgsl/kgsl-3d0/gpuclk, or devfreq/cur_freq) + g64prof gpuWait in
   each + the per-pair fence timing. Plus the copies-on VISUAL sweep on gameplay
   and other menus (Adreno-specific breakage untested; rig sweep was clean).
   Decides: fixed-latency -> scope linear-loadBlock path vs submit-loop rework;
   and whether DVFS is truly out.
FIRST RE QUESTION after the captures: the scratch's full lifecycle — do the 12
format-change fences (rt64_state.cpp:558) share origin with its w=8->w=256 re-spec
each cycle? If so, one fix may resolve both the loadBlock decline AND the
format-change fences (handover §4 #3). Nuance: enumeration shows fmt/siz constant
across re-specs, so the link would be via width/re-spec, needs confirming.

ENVIRONMENT / REPRO:
- Desktop rig (structural counts + logic only; llvmpipe software Vulkan so ALL rig
  TIMING IS INVALID — device is authoritative for ms):
  cmake -B build-desktop -G Ninja -DCMAKE_BUILD_TYPE=Debug
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ [-DRT64_PROFILE_LOGCAT=1]
  clang is LOAD-BEARING (makes RECOMP_FUNC weak; GCC -> multiple-def link error).
- Runtime ROM gotcha: repo mnsg.z64 is the DECOMPRESSED build-time ROM
  (hash 0x380FC5D167CE89BA), NOT valid at runtime. Runtime wants mnsg.us.z64
  (16MiB, 0xDB1BC7EE0E6BEBA1) from the device backup, staged in
  ~/.config/Goemon64Recompiled/. Inspector: developer_mode:true in graphics.json
  (F1). copies-on A/B on desktop: G64_COPY_GPU=1 (env; won't work on Android — a
  device copies-on build needs a hardcoded flip).
- Android build: cd android && ./gradlew assembleDebug (after
  source ~/goemon-buildenv.sh); install via Windows adb; capture
  adb logcat -s Goemon64-stdio | grep -E 'g64prof|g64tile'. Current APK
  (g64prof+enum, copies-off): android/app/build/outputs/apk/debug/app-debug.apk.
- rt64 is CHECKED OUT on diag/menu-framerate (has instrumentation; parent gitlink
  stays 8c73b3f release line so CI builds clean). git -C lib/rt64 checkout
  goemon-android for a non-instrumented build.
- Provenance (all PUSHED): rt64 diag 6c07782 (g64prof) / 7e49e1f (enum) /
  fa91fd6 (tile trace) on ogdanimal/rt64. origin/dev in sync at 6327cd8
  (FILE_TO_C build fix c4d896b, G64_COPY_GPU toggle a82e44d, docs).

SEPARATE THREADS — do NOT conflate with the menu stall:
- Gameplay 30 FPS cap: exonerated from the fence path (pairSync ~4.7ms of 33ms),
  ZERO evidence gathered; its own investigation if ever picked up.
- plume recreateSurface UAF: the one small Vulkan pre-public-flip item, separate
  from framerate.
```
