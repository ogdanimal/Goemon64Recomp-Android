# RESUME — Issue #15, white screen on Mali GPUs

Handoff note for a fresh session. **Root cause found and fixed 2026-07-23,
verified on Mali hardware. The fix is committed on `dev` but not released, and
the issue has not been answered yet.**

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

First **non-Adreno** report. Every device tested before was Adreno (RP5 = Adreno
650, AYN Thor = SD 8 Gen 2), which is exactly why it went unseen.

**DECIDED 2026-07-23: do not reply until the fix ships.** Do not re-raise
without the user.

## ROOT CAUSE

**RT64 implements N64 alpha blending entirely with dual-source blending, and no
Mali GPU supports the `dualSrcBlend` feature.**

- `rt64_raster_shader.cpp` set `srcBlend = SRC1_ALPHA`, `dstBlend =
  INV_SRC1_ALPHA` **unconditionally**. The comment there says why: the blend
  factor rides in the secondary output so `SV_TARGET0.a` stays free to carry N64
  **coverage** (`RasterPS.hlsl`, `resultAlpha.a = resultColor.a`).
- plume passes the device's own queried feature set straight to `vkCreateDevice`
  (`plume_vulkan.cpp:4112` → `:4269`), so a feature is enabled iff supported.
  Mali does not support `dualSrcBlend`, so it was not enabled.
- **The Mali driver still returns `VK_SUCCESS`** from
  `vkCreateGraphicsPipelines` for the invalid pipeline and then blends
  undefined. Zero `vkCreateGraphicsPipelines failed` lines in any log.

That is the whole "every API call succeeds, the output is wrong" shape. The
giveaway in the frames: geometry rasterised **perfectly** (the Konami logo is
pin-sharp and correctly placed) but everything was binarised to pure black on
pure white. Shape survived, colour was annihilated — blending, not geometry and
not textures.

## THE FIX

Gated on a new capability, so devices that support dual-source blending take the
byte-identical old path. Inert on Adreno, desktop Vulkan, D3D12 and Metal.

- plume `4e77e67` — `RenderDeviceCapabilities::dualSrcBlend`, set from the
  queried Vulkan feature, hardcoded true on D3D12/Metal, and added to the
  Android startup diagnostic line.
- rt64 `099f852` — a `NO_DUAL_SRC_BLEND` variant of `RasterPS.hlsl` that drops
  the `SV_TARGET1` output and writes the blend factor into the primary output's
  alpha; six new SPIR-V variants in `CMakeLists.txt`; runtime selection in
  `RasterShader` / `RasterShaderUber`; and `SRC_ALPHA`/`INV_SRC_ALPHA` at the
  single `createPipeline` site both paths funnel through.

**TRADEOFF, by design:** on the fallback path the coverage value the primary
output would normally carry is lost for alpha-blended draws, so coverage-based
effects (N64 anti-aliased edges) are approximate. This is an approximation, not
an equivalent. It is a large improvement over a white screen.

### What was verified, and what was not

VERIFIED on the Galaxy A15 (Mali-G57 MC2, r38p1):

- Startup logs `dualSrcBlend=0` — direct confirmation, not inference.
- Intro and title screen render in **full colour** with textures, sky gradient,
  transparency and text. Frames went from a flat 12,499-byte white fill to
  200–560 KB of real content.
- Vulkan validation reports **0** dual-source errors where it previously
  reported **72** (36 `srcColorBlendFactor-00608` + 36
  `dstColorBlendFactor-00609`). That zero is trustworthy because the **same run
  still reports the other two VUID classes** — the layer was demonstrably live.

NOT verified:

- **No Adreno smoke test.** The RP5 was not connected. The gate is
  unchanged-by-construction when `dualSrcBlend` is true, but **smoke-test on
  Adreno before cutting a release.**
- The reporter's Mali-G77 specifically. Symptom and vendor match; not proven.

## Repro device and how to connect

**Galaxy A15 5G** — `SM-A156U1`, SoC `MT6835` (Dimensity
6100+), **Mali-G57 MC2, GPU driver r38p1**, Android 15 (SDK 35), Vulkan 1.3,
arm64-v8a.

**Wireless debugging ONLY. The USB path is a dead end — do not retry it.**
Windows enumerates the ADB interface, but the node has an empty
`DeviceInterfaceGUIDs` with `ExtPropDescSemaphore = 1`, so adb's GUID search
finds nothing and the device never even shows as `unauthorized`.

```bash
ADB=/mnt/c/Users/$USER/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe
"$ADB" mdns services          # discovers BOTH the pairing and connect ports
CONN=$("$ADB" mdns services 2>/dev/null | awk '/_adb-tls-connect/ {print $3}' | head -1)
"$ADB" connect "$CONN"
```

**The port rotates on every reconnect** (seen 36659 → 40273 → 45441 → …). Always
re-derive it. The device also enumerates **twice** — once by `IP:port`, once by
mDNS service name — as one physical device, so `adb` will say "more than one
device/emulator"; **pin it with `adb -s <ip:port>`**.

## Vulkan validation layers — already installed, no rebuild needed

This is what found the root cause in one step, after several sessions of
guessing. Reach for it early next time.

The layer `.so` is already in the app's data dir on the A15. Toggle it with:

```bash
adb shell settings put global enable_gpu_debug_layers 1   # or 0
adb shell settings put global gpu_debug_app com.goemon64.recomp
adb shell settings put global gpu_debug_layers VK_LAYER_KHRONOS_validation
```

Output lands in logcat under tag `VALIDATION` (and mirrored into
`Goemon64-stdio`). To reinstall it from scratch: grab
`android-binaries-*.zip` from the Vulkan-ValidationLayers GitHub releases, then
`adb push` the arm64-v8a `.so` to `/data/local/tmp` and
`adb shell run-as com.goemon64.recomp cp /data/local/tmp/lib….so .` — this works
because the debug build is debuggable.

**Leave a note if you turn it on:** it is a global device setting and it slows
the app noticeably. It is currently **enabled**.

## STILL OPEN — found by validation, neither causes the white screen

- **40× `VUID-vkCmdDraw-renderPass-02684`** — renderpass/pipeline format
  mismatch, `R8G8B8A8_UNORM` vs `B8G8R8A8_UNORM`. This is fallout from our own
  Android swapchain-format change (`rt64_application.cpp:323`), which left two
  hardcodes behind: `src/ui/ui_renderer.cpp:106` and
  `rt64_shader_library.cpp:602` (the latter carries an upstream `TODO: Use
  whatever format the swap chain was created with`). **The drift-proof fix is a
  `getFormat()` on plume's `RenderSwapChain`**, not a third copy of the
  `#ifdef __ANDROID__` — the duplication is what caused this. Vulkan's swapchain
  already stores `pickedSurfaceFormat`; D3D12/Metal would need the requested
  format stored at creation.
- **4× `VUID-VkGraphicsPipelineCreateInfo-renderPass-06041`** — `blendEnable`
  is true on an `R32G32B32A32_SFLOAT` attachment that Mali cannot blend.

## What is REFUTED — do not re-derive these

- **`hpfb_option` is NOT causal.** An earlier session recorded a "confirmed
  causal, A→B→A, byte-identical frames" result for `hpfb_option`. It does not
  reproduce. `Off`/`On`/`Auto` all render the launcher correctly, and in-game
  **all four** combinations of `hpfb` × `res_option` produce byte-identical
  white (12,499 B). The likely mistake: comparing the **RmlUi launcher** against
  the **RT64 game** as if they were the same surface. They are not — a correct
  menu says nothing about the renderer.
- **`res_option` is not causal** — `Original` and `Original4x` are identical.
- **The `mali_gralloc` "Unrecognized and/or unsupported format 0x38/0x3b" errors
  are noise.** They appear identically in launcher runs that render correctly.
- **The descriptor-set theory.** The predicted failure of the Android-only
  full-`UpperRange` (8192) `UPDATE_AFTER_BIND` texture-set allocation in
  `rt64_framebuffer_renderer.cpp` does not occur — a clean log has **no**
  `vkAllocateDescriptorSets failed`.
- **`shaderInt64=0`** is a red herring; no RT64 shader uses 64-bit ints.

## Gotchas that already cost time

- **Do NOT push the repo-root `mnsg.z64`.** It is the 32 MiB *decompressed build
  input* (`6ea0ed71…`, what CI pins). The app validates the player ROM against
  the **original 16 MiB cart** (`df8083a5…`, `LauncherActivity.java:36`). Correct
  copies live in `%USERPROFILE%\goemon-backups\*\data\mnsg.us.z64`. Already
  documented in memory `device-install-method` and walked into anyway.
- **`BUILDING.md:49` is wrong** — it labels `df8083a5…` as the "decompressed ROM
  sha1" and says to copy it to the repo root. That is the runtime cart hash, not
  the build input. Known, unfixed.
- **`adb install` takes a HOST path**, not a device path.
- **Screenshot size is a fast triage signal**: a flat fill compresses to ~12 KB,
  real content to 100 KB+. Compare sha1s rather than eyeballing frames.
- **The launcher menu is RmlUi, the game is RT64.** A correct menu does not mean
  the renderer is healthy. Confusing the two is what produced the bogus `hpfb`
  finding above.
- **Drive the menu over adb** with `input keyevent 66` (Enter) to reach Start
  Game without touching the device.
