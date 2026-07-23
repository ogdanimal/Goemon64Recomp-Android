# RESUME — Issue #15, white screen on Mali GPUs

Handoff note for a fresh session. Written 2026-07-23, at the point where the bug
is **reproduced on hardware we control** but the root cause is **not yet found**.

## Read these first

- CLAUDE.md § "Current focus" — the issue #15 bullet is the authoritative summary
- Memory `goemon-issue-15-mali-white-screen` — same content, session-portable
- Memory `mali-repro-device-a15` — the test device and how to reach it
- Memory `device-install-method` — APK/ROM install mechanics (read the ROM
  warning before pushing anything)

## The report

GitHub issue **#15** on `ogdanimal/Goemon64Recomp-Android`, opened 2026-07-23 by
`tonysantosl`. **Retroid Pocket 4 Pro** (Dimensity 1100, **Mali-G77 MC9**):

> The game boots normally and the sound is fine, but graphics are all white with
> most textures not loading at all. It probably has something to do with the Mali
> processor.

Two screenshots attached. No app version stated — never asked, because we decided
to hold the reply.

This is the first **non-Adreno** report. Every device we had tested on before is
Adreno (RP5 = Adreno 650, AYN Thor = SD 8 Gen 2), which is why it went unseen.

**DECIDED 2026-07-23: do not reply on the issue until we have a fix.** No interim
"we reproduced it" comment. Do not re-raise without the user.

## Repro device and how to connect

**Galaxy A15 5G** — `SM-A156U1`, SoC `MT6835` (Dimensity
6100+), **Mali-G57 MC2, GPU driver r38p1**, Android 15 (SDK 35), Vulkan 1.3,
arm64-v8a.

Not the same GPU as the reporter's G77, but the same vendor, the same Valhall
generation, and — the part that matters — a **similar-era ARM driver** (r38p1 is
a ~2022 DDK; a 2024 budget MediaTek phone shipping Android 15 on it is normal).

**Wireless debugging ONLY. The USB path is a dead end — do not retry it.**
Windows enumerates the ADB interface, but the node has an empty
`DeviceInterfaceGUIDs` with `ExtPropDescSemaphore = 1`, so adb's GUID search
finds nothing and the device never even shows as `unauthorized`.
`pnputil /remove-device` + rescan did not clear it, and the interface number
shifts between plugs. Untried remaining options are a registry GUID injection or
Samsung's USB driver; not worth the time.

```bash
ADB=/mnt/c/Users/$USER/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe
"$ADB" mdns services          # discovers BOTH the pairing and connect ports
CONN=$("$ADB" mdns services 2>/dev/null | awk '/_adb-tls-connect/ {print $3}' | head -1)
"$ADB" connect "$CONN"
```

**The port rotates on every reconnect** (seen 36659 → 40273 → 45441). Always
re-derive it from `adb mdns services`; a hardcoded port gives `device offline` or
`device not found`. Pairing itself persists across reboots — if it ever needs
re-pairing, only the 6-digit code has to come from the user (tap the *label*
"Wireless debugging", not the toggle, to reach the pairing screen).

## State of the device right now

Ready to resume with no setup:

- Debug APK installed (`com.goemon64.recomp`), local build of the 2026-07-21 tail
- Correct 16 MiB ROM registered and checksum-verified in app storage
- `graphics.json` currently has `hpfb_option: Off`
- A stale 32 MiB `mnsg.z64` is still in `/sdcard/Download` — deletable

Config lives at
`/storage/emulated/0/Android/data/com.goemon64.recomp/files/data/` (internal —
this phone has no SD card in play). Push a modified `graphics.json` there and
force-stop + relaunch to apply; no UI navigation needed.

## What is ESTABLISHED

**Reproduced, cleanly.** Flat white screen on the A15, with the correct ROM
(`df8083a5…`) checksum-verified in app storage, so it is not a ROM-mismatch
artifact.

**`hpfb_option` is causal for the launcher menu — A→B→A, byte-identical frames:**

| Run | `hpfb_option` | Result | screenshot sha1 |
|-----|---------------|--------|-----------------|
| 1   | `Auto`        | flat white | `d3b2ab4c…` |
| 2   | `Off`         | **menu renders correctly** | `cfdac1ce…` |
| 3   | `Auto`        | flat white | `d3b2ab4c…` ← identical to run 1 |

Two independent `Auto` runs produced byte-identical white frames. That is a real
isolation, not a lucky pass.

**`hpfb=Off` does NOT fix the game.** Start Game gives a black silhouette on
white during the intro, then settles back to flat white. So high-precision
framebuffer is a contributing factor, **not the root cause** — or there are two
separate faults. **It is not a user-facing workaround; do not offer it as one.**

**Startup is otherwise clean** — swapchain created, fonts loaded, no Vulkan
error, no crash, no tombstone. The shape of this bug is "every API call
succeeds, the output is wrong."

## What is REFUTED

**The descriptor-set theory. Do not re-raise without new evidence.** The
prediction was that the Android-only branch in
`lib/rt64/src/render/rt64_framebuffer_renderer.cpp` — which allocates the texture
set at the full `UpperRange` (8192, `rt64_descriptor_sets.h:294`)
`UPDATE_AFTER_BIND` capacity, a workaround we wrote for Adreno — would exceed
Mali's `maxDescriptorSetUpdateAfterBindSampledImages`, leaving
`VulkanDescriptorSet::vk` null (`plume_vulkan.cpp:1941`) so nothing binds and
everything renders untextured. It fit the symptom exactly.

A clean log contains **no** `vkAllocateDescriptorSets failed`. The theory is
dead. It was elaborated across several turns before anyone checked the one log
line that would have killed it in one step.

## Observations of UNESTABLISHED significance

- `shaderInt64=0` and `vertexPipelineStoresAndAtomics=0` on this Mali. But plume
  only **logs** these (`plume_vulkan.cpp:4115`) — it passes the full supported
  feature set to `vkCreateDevice` and never requires them — and no RT64 shader
  uses 64-bit ints. `shaderInt64` is probably a red herring;
  `vertexPipelineStoresAndAtomics` is not ruled out.
- `E mali_gralloc: Unrecognized and/or unsupported format 0x38 / 0x3b` fires at
  swapchain enumeration. Likely benign driver format-probing noise — plume's
  requested format 37 (`R8G8B8A8_UNORM`) *is* in the offered list — but this has
  **not** been re-checked under `hpfb=Off`.

Reference startup lines from a clean run:

```
[plume] Vulkan loader supports 1.3.0, requesting 1.2.
[plume] Using device "Mali-G57 MC2" (vendor 0x13B5, device API 1.3.219, driver 0x09801000).
[plume] storageImageReadWithoutFormat=1 writeWithoutFormat=1 shaderInt64=0
        fragmentStoresAndAtomics=1 vertexPipelineStoresAndAtomics=0
[plume] requested swapchain format=37; 5 surface formats offered: 37 43 4 97 64
```

## NEXT, in order

1. **Find what `hpfb: Auto` actually resolves to on this device** and why it
   poisons the output. This is the strongest thread: a proven behavioral hook,
   and it is in *our* code, not the driver's.
2. **Enable Vulkan validation layers** against the live repro. We have a
   reproducible failure on demand; validation would likely name the invalid usage
   outright rather than us guessing.
3. **Test `res_option: Original`.** Android pins `Original4x`
   (`src/game/config.cpp`), so a supersampled-framebuffer interaction is
   unexcluded.
4. **Re-check the gralloc format errors under `hpfb=Off`** to settle whether they
   are noise.

## Gotchas that already cost time

- **Do NOT push the repo-root `mnsg.z64`.** It is the 32 MiB *decompressed build
  input* (`6ea0ed71…`, what CI pins). The app validates the player ROM against
  the **original 16 MiB cart** (`df8083a5…`, `LauncherActivity.java:36`). Correct
  copies live in `%USERPROFILE%\goemon-backups\*\data\mnsg.us.z64`. This was
  already documented in memory `device-install-method` and got walked into anyway.
- **`BUILDING.md:49` is wrong** — it labels `df8083a5…` as the "decompressed ROM
  sha1" and says to copy it to the repo root. That is the runtime cart hash, not
  the build input. Known, unfixed.
- **`adb install` takes a HOST path**, not a device path. Pushing the APK to
  `/sdcard/Download` and installing from there fails with `failed to stat`.
- **Screenshot size is a fast triage signal**: a flat fill compresses to ~12 KB,
  real content to 100 KB+. Compare screenshot sha1s to prove two runs produced
  the identical frame rather than eyeballing them.
- The launcher menu is **RmlUi**, the game is **RT64**. A correct menu does not
  mean the renderer is healthy — that distinction is exactly what `hpfb=Off`
  exposed.
