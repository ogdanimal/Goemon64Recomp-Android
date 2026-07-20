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

WHERE THINGS STAND (do not re-litigate the fence-POLICY fixes — measured dead;
but copies-on is a real MITIGATION, see fix (a) below):
- Symptom: Select Adventure Diary screen runs ~14 FPS (gameplay is 30). Device is
  a Retroid Pocket 5 (Snapdragon 865 / Adreno 650), NOT rooted.
- Root cause, MEASURED: copyWithGPU=false (src/main/rt64_render_context.cpp:356)
  forces one blocking GPU submit+fence per framebuffer pair; this screen emits ~20
  pairs/frame (10x gameplay's 2) → ~46ms of serialized gpuWait at a DVFS-floored
  305MHz ≈ the whole frame. Not CPU-bound, not copy-bound (cpuCopy ~0.4ms).
- Fix (a) copies-on: crossed off as a SERIALIZATION CURE (fences 20→19, gpuWait
  unchanged) BUT retained as a real MITIGATION — device A/B is 14.0→17.4 FPS
  (+24%), glitch-free on this screen, from CPU-side command-build savings. The
  desktop-rig 'copies-on is slower' reading is a llvmpipe artifact, NOT device
  behaviour (it emulates GPU copies on CPU). Cheapest evidenced smoothness lever
  for mnsg; needs a copies-on validation pass on gameplay/other screens (never
  captured) before shipping. G64_COPY_GPU=1 is the toggle. See results doc
  'What actually remains' lever 1.
- Fix (b) precise barrier: REFUTED. The copies-off naturalFences=12 is a DETECTOR
  ARTIFACT — checkFramebufferOverlap early-returns when copies are off
  (rt64_rdp.cpp:139), so framebuffer-as-texture deps go undetected and the blanket
  (f>0) override is load-bearing for correctness, not paranoia. True dep count ~18,
  so precise fencing can't beat copies-on's ~17 FPS. Naive (b) is a correctness bug.
- REMAINING LEVERS, best-evidenced first: (a) ship copies-on for mnsg [+24%,
  needs validation pass], DVFS/clock [zero-build, user-side], (c) reduce the ~20
  pair count [large engine RE].

NEXT SESSION OPENING MOVE:
1. DONE (2026-07-19). The desktop build now builds AND launches; the on-ramp is
   retired. Turnkey: `cmake -B build-desktop -G Ninja -DCMAKE_BUILD_TYPE=Debug
   -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++` then build target
   Goemon64Recompiled. Both blockers resolved (see the results doc's on-ramp
   section, now marked RESOLVED):
   a) FILE_TO_C: FIXED permanently in CMakeLists.txt (desktop else() now sets
      FILE_TO_C=file_to_c). No -D flag needed.
   b) multiple-definition: it was a COMPILER issue, not a missing patch-exclusion.
      RECOMP_FUNC is weak under clang (extern inline __attribute__((weak))) and
      strong under gcc; the failed smoke-test used /usr/bin/cc. Build with clang
      (the -DCMAKE_*_COMPILER flags above) → clean link, no --allow-multiple-def.
   Vulkan/WSLg RETIRED: it runs, but on llvmpipe (SOFTWARE Vulkan), not GPU
   passthrough — fine for the dependency-structure RE, slow to reach the screen,
   and desktop absolute timings are NOT comparable to the device.
2. NOW THE ACTUAL WORK — inspector RE (interactive, user-driven on a real desktop
   session): launch, pick ROM, load a save, reach the Select Adventure Diary
   screen, press F1, read per-pair syncRequired. Question: why ~20 framebuffer
   pairs on this screen, can they merge? (developerMode wired to debug at
   rt64_render_context.cpp:346.)

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
