# Vulkan device requirements — what the port actually needs

Status: **PARKED** (2026-07-18). Nothing here is device-verified; every claim below is
static evidence from the tree. Recorded so the eventual experiment tests the right thing.

Motivation: working out which Android devices can run the port, and what the manifest
`uses-feature` version should be.

## The declared floor is not the real floor

- `plume_vulkan.cpp` sets `appInfo.apiVersion = VK_API_VERSION_1_2`.
- `AndroidManifest.xml` declares `android.hardware.vulkan.version` `0x400003` (= 1.0.3).

Neither number reflects what the renderer needs, and neither is enforced:

- **`appInfo.apiVersion` is not a compatibility check.** Since Vulkan 1.1 the spec forbids
  failing instance creation because the requested apiVersion is higher than supported.
  Only a strict 1.0 loader returns `VK_ERROR_INCOMPATIBLE_DRIVER`, and `minSdk 28`
  (Android 9+) guarantees a 1.1+ platform loader. So requesting 1.2 does **not** exclude
  1.1 devices.
- **`uses-feature` is Play Store filtering only.** It is not enforced by `pm install` or
  sideloading. **This port is sideload-only and there is no planned Play Store release**
  (the parked release work is signed *GitHub* Releases, which is still sideloading), so
  this value is permanently inert. Do not spend effort "correcting" it — its value cannot
  affect any user. Left at `0x400003` deliberately.

## What the shaders need: Vulkan 1.0

- `lib/rt64/CMakeLists.txt:136` — every SPIR-V compile uses `-fspv-target-env=vulkan1.0`.
- `CMakeLists.txt:181` — the Android compute path runs `spirv-val --target-env vulkan1.0`,
  so the build itself asserts 1.0 validity.
- `CMakeLists.txt:142` — the `vulkan1.1spirv1.4` flags belong to `build_ray_shader`
  (defined :353), which is never called; `RT_ENABLED` is never defined, so the RT path is
  compiled out.
- Verified against artifacts, not just flags: all 80 `.spv` files under
  `android/app/.cxx/.../arm64-v8a/` have header version `0x00010000` (SPIR-V 1.0).

So the SPIR-V-version hypothesis for a 1.2 floor is **ruled out**.

## What actually gates the device: descriptor indexing, unconditionally

This is the real requirement, and it is a **feature list, not an extension name** — the
features of `VK_EXT_descriptor_indexing` are individually optional.

Required in practice:
- `descriptorBindingPartiallyBound`
- `descriptorBindingVariableDescriptorCount`
- `runtimeDescriptorArray`
- `descriptorBindingSampledImageUpdateAfterBind`  ← **not probed anywhere**

Evidence:
- `rt64_descriptor_sets.h:294,304` — boundless 8192-entry range, unconditional.
- `shaders/FbRendererCommon.hlsli:43,46` — `Texture2D gTextures[8192]`, indexed with
  `NonUniformResourceIndex` (`TextureSampler.hlsli:103,107,170,275`).
- `plume_vulkan.cpp:1198-1208` — sets `UPDATE_AFTER_BIND | PARTIALLY_BOUND |
  VARIABLE_DESCRIPTOR_COUNT` purely on `lastRangeIsBoundless`, with no capability branch.

### Inconsistency worth fixing (also upstreamable)

plume treats descriptor indexing as **optional** — it is in `OptionalDeviceExtensions`
(`plume_vulkan.cpp:75-92`), features are chained conditionally, and `RequiredDeviceExtensions`
is only `VK_KHR_swapchain` (:71-73). rt64 above plume **assumes** it: the only references to
`capabilities.descriptorIndexing` in the entire tree are two debug-overlay prints
(`rt64_state.cpp:2555-2556`). Nothing branches on it.

So plume's fallback path is dead code in this configuration, and a device that fails the
probe proceeds anyway.

Two further gaps:
- plume's probe (`:4089`) checks partially-bound, variable-count and runtime-array but
  **not** `descriptorBindingSampledImageUpdateAfterBind`, which :1198-1208 requires. A device
  can pass every check plume makes and still violate the layout-creation VUID.
- Using those flags without the feature enabled is **undefined behaviour**, not a guaranteed
  clean `VkResult`. Expect "errors on some drivers, accepts and corrupts later on others" —
  do not assume a diagnosable failure.

## Buffer device address — the 1.1 hazard is the inverse of the obvious one

Not a blocker: probed optional (`:4029`), VMA flag set conditionally (`:4253`), non-RT use
site gated on `capabilities.bufferDeviceAddress` (`:839`), remaining call sites are in dead
RT paths. A 1.1 device *without* the extension never reaches them.

The actual risk is a device that **has** `VK_KHR_buffer_device_address`: plume calls the
core-named `vkGetBufferDeviceAddress`, and a pre-1.2 driver may export only
`vkGetBufferDeviceAddressKHR` → null core pointer → crash on first use. Lowering
`appInfo.apiVersion` does not address this.

The one thing genuinely tied to `appInfo.apiVersion` is VMA: `vulkanApiVersion` (`:4258`)
must not exceed what the device supports. Standard remedy is to clamp at runtime to
`min(loaderVersion, physicalDeviceProperties.apiVersion)` rather than hard-coding it down.

## Other latent issue

`plume_vulkan.cpp:4135-4162` `pickFamilyQueue` — `familyIndex` initialises to 0 with no
found-flag, so a no-match silently binds queue family 0 instead of failing. Unlikely to
matter on mainstream drivers; would be hard to diagnose if it did.

## If this is ever unparked

**Read this first: the original motivation is gone.** This investigation started as "what
should the manifest `uses-feature` version be?" — a question that cannot matter for a
sideload-only port. Do not resume it for that reason.

What could still justify the work, in descending order of actual value:

1. **The plume/rt64 descriptor-indexing inconsistency** (above). A real defect affecting
   real devices, needs no 1.1 hardware to justify, and is upstreamable since it is in
   plume/rt64 rather than Goemon-specific code. **This is the only item here worth doing
   on its own merits.**
2. A runtime capability check that shows a clear "your device lacks X" message instead of
   the current undefined behaviour. Needs the feature list above, not a version number.
3. Knowing what to tell users about compatibility. Nice to have; the diagnostics logged in
   `VulkanInterface`/`VulkanDevice` already cover most of it in bug reports.

Only item 2 needs the experiment below, and only to confirm the message fires correctly.

The load-bearing unknown is whether the code as written runs on a Vulkan 1.1 device that
has the four descriptor-indexing features. Static reading cannot settle it.

Order matters — the cheap precondition first:
1. Get a Vulkan 1.1 instrument and run `vulkaninfo` on it to confirm it reports the four
   features above. **SwiftShader was assumed to be a valid instrument; that is unverified.**
   Its reported version is release-dependent (recent builds advertise 1.3) and its
   descriptor-indexing support has historically been patchy. Confirm before investing.
   Note: as of this writing there is **no emulator, AVD, or system image installed** in
   this SDK, so this step is itself a setup task, not a quick check.
2. Only if (1) passes: build x86_64 (`abiFilters` is arm64-only today; `README.md:10` lists
   x86_64 as planned) and run the port unmodified to see whether a 1.1 loader with a 1.2
   `appInfo` works.
3. Then try clamping `appInfo.apiVersion`/VMA to the device version.
4. (Manifest: nothing to do. See the sideload-only note above.)

Fixing the plume/rt64 inconsistency above is worthwhile independently of any of this, and
does not need a 1.1 device to justify.
