# Resume prompt — Android menu-framerate investigation

Paste the block below as the first message of a fresh session. Full written record is in
[`docs/android-profiling-results.md`](../android-profiling-results.md) (committed `e1641f7` on `dev`)
and [`docs/android-profiling-plan.md`](../android-profiling-plan.md) — **read the results doc first;
it is authoritative and this prompt is its summary, not a substitute.**

---

```
Resume the Goemon64Recomp Android menu-framerate investigation. Full written
record is in docs/android-profiling-results.md (committed e1641f7 on dev) and
docs/android-profiling-plan.md — READ android-profiling-results.md FIRST; it is
authoritative and the facts below are its summary, not a substitute.

WHERE THINGS STAND (do not re-litigate — both fence fixes are refuted by evidence):
- Symptom: Select Adventure Diary screen runs ~14 FPS (gameplay is 30). Device is
  a Retroid Pocket 5 (Snapdragon 865 / Adreno 650), NOT rooted.
- Root cause, MEASURED: copyWithGPU=false (src/main/rt64_render_context.cpp:356)
  forces one blocking GPU submit+fence per framebuffer pair; this screen emits ~20
  pairs/frame (10x gameplay's 2) → ~46ms of serialized gpuWait at a DVFS-floored
  305MHz ≈ the whole frame. Not CPU-bound, not copy-bound (cpuCopy ~0.4ms).
- Fix (a) copies-on: REFUTED by an on-device A/B (fences 20→19, gpuWait unchanged).
- Fix (b) precise barrier: REFUTED. The copies-off naturalFences=12 is a DETECTOR
  ARTIFACT — checkFramebufferOverlap early-returns when copies are off
  (rt64_rdp.cpp:139), so framebuffer-as-texture deps go undetected and the blanket
  (f>0) override is load-bearing for correctness, not paranoia. True dep count ~18,
  so precise fencing can't beat copies-on's ~17 FPS. Naive (b) is a correctness bug.
- REMAINING LEVERS: (c) reduce the ~20 pair count [structural RE], and DVFS/clock.

NEXT SESSION OPENING MOVE:
1. Fix the desktop build so the RT64 inspector (visualizes per-pair syncRequired,
   developerMode wired to debug at rt64_render_context.cpp:346, F1) can drive the
   option-(c) RE. build-desktop/ is left at 790/848 (incremental). TWO blockers,
   both detailed in the doc's "Desktop-inspector on-ramp":
   a) FILE_TO_C unset on the desktop CMake branch — workaround that WORKED:
      -DFILE_TO_C="$PWD/build-host-tools/file_to_c". Proper fix: desktop else()
      branch (~CMakeLists.txt:487) should set FILE_TO_C to the native file_to_c.
   b) multiple-definition at link: every RECOMP_PATCH'd func is in BOTH
      RecompiledPatches/patches.c and RecompiledFuncs/funcs_*.c; the Android build
      has a patch-exclusion/weak-symbol mechanism the desktop path lacks. NOT yet
      worked around — this is the real first task. (--allow-multiple-definition
      would let the patch win by link order but risks a subtly wrong binary.)
2. Then inspector RE: why 20 framebuffer pairs on this screen, can they merge?

PENDING USER-SIDE EXPERIMENT (dev-device, zero build): flip the Retroid firmware
performance mode (raises the DVFS floor; sysfs pin is impossible — no root) and
re-capture the diary screen. If gpuWait drops materially, much of the 46ms is
clock-inflation and (c) over-delivers; if not, the pairs are genuine GPU work.

ENVIRONMENT / TOOLING:
- Windows adb (WSL sees no device): /mnt/c/Users/user/AppData/Local/Microsoft/
  WinGet/Packages/Google.PlatformTools_*/platform-tools/adb.exe
- Instrumentation is on rt64 branch diag/menu-framerate (6c07782, pushed to
  ogdanimal/rt64), behind RT64_PROFILE_LOGCAT (auto-on for __ANDROID__). The rt64
  submodule is CHECKED OUT on that branch right now (parent gitlink untouched at
  8c73b3f); `git -C lib/rt64 checkout goemon-android` restores the clean release
  line. Local Android build: cd android && ./gradlew assembleDebug (after
  `source ~/goemon-buildenv.sh`); install -r -d via the Windows adb.
- Capture: adb logcat -s Goemon64-stdio | grep g64prof  (fullSync + render lines).
- Desktop build deps already apt-installed: libsdl2-dev libfreetype-dev
  libvulkan-dev libgtk-3-dev. sudo IS available here.

QUARANTINED, DO NOT CONFLATE: gameplay's 30 FPS cap is formally exonerated from
the fence path (pairSync ~4.7ms of a 33ms frame) and has NO evidence attached —
it's a separate investigation if ever picked up.
```
