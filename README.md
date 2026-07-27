# Goemon 64 Recompiled — Android

An Android port of [Goemon 64: Recompiled](https://github.com/klorfmorf/Goemon64Recomp) — plays *Mystical Ninja Starring Goemon* natively on your phone or handheld, using [RT64](https://github.com/rt64/rt64) for Vulkan rendering.

This is a packaging/port layer on top of the upstream recompilation project (built on [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp)). For how the recompilation itself works, or for desktop builds, see the upstream repo.

## Download

Get the APK from the releases page:

**https://github.com/ogdanimal/Goemon64Recomp-Android/releases**

The APK does not include the game. You'll need your own legally obtained ROM.

## Getting Started

1. Install the APK and open the app.
2. On first launch, you'll be asked to pick your ROM file — use the file picker, it gets copied into the app's own storage.
3. Make sure you have a gamepad. **The game is controller-only** — there is no touchscreen control scheme. A handheld's built-in controls work as-is; on a phone, pair a physical or Bluetooth pad first.
4. Press Start.

**Requirements:** Android 9.0+, a 64-bit (`arm64-v8a`) device, and a Vulkan-capable GPU. This covers effectively any phone or handheld from the last several years. Tested primarily on Snapdragon/Adreno handhelds (Retroid Pocket 5, AYN Thor).

## Something's Wrong — Quick Fixes

| Problem | Try this |
|---|---|
| Game closes itself a few seconds after starting | Known bug in some Qualcomm/Adreno drivers. Go to **Settings → Graphics → Framebuffer Effects** and turn it off, *or* load an alternate driver (see [GPU Driver](#gpu-driver) below). |
| White screen, textures missing | Fixed as of `1.0.3` — update to the latest release. |
| Some controller buttons do nothing, and don't respond when rebinding | Android doesn't recognize your pad's exact model. Fixed for DualSense Edge on Android 12 and earlier as of `1.0.4`; for anything else, [open an issue](../../issues) with your pad model and Android version. |
| Still stuck | [Open an issue](../../issues) with your device model and Android version. |

More detail on each of these is in [Troubleshooting Details](#troubleshooting-details) below.

## Default Controls

The default gamepad layout (Xbox-style face buttons). Everything is remappable in **Settings → Controls**.

| Button | N64 | In-game action |
|---|---|---|
| A | A | Jump |
| B | B | Attack |
| X | C-Up | Magic |
| Y | C-Left | Weapon swap |
| Right Bumper | C-Down | Change character |
| Left Trigger | Z | Crouch |
| Right Trigger | R | Camera control (zoom modifier when Analog Camera is on) |
| Left Bumper | L | Unused by the game (free for mods) |
| D-Pad ↑ ↓ ← → | C-Up / C-Down / C-Left / C-Right | Magic / Character / Weapon / Map |
| Left stick | Analog stick | Move |
| Right stick | C-buttons | Drives the camera instead when Analog Camera is on |
| Right stick click (R3) | — | Recenter the analog camera |
| Start | Start | Pause |
| Select | — | Open this app's settings menu |

The right stick and D-Pad both cover the C-buttons, so with Analog Camera on, the right stick orbits while the D-Pad keeps the C-button actions.

## Optional Features

Everything below is opt-in and lives in the in-game settings menu.

- **Analog Camera** — free-look with the right stick. Hold **R** + push the stick to zoom. Per-axis invert and sensitivity settings included. Click **R3** to hand control back to the game.
- **Swap Characters While Moving** — swap (C-Down) without stopping first. Doesn't speed up the swap itself, just removes the need to stand still.
- **Attack While Moving** — keep moving through a ground attack instead of rooting in place. Purely cosmetic; Goemon's chain pipe is unaffected since it anchors to a fixed point.
- **Autosave** — saves through the game's own save system (so it's an ordinary save, not a separate slot). **Back up your save before enabling this** — it overwrites your existing file. Manual save: **L + R + Z** during normal gameplay.
- **Cheats** — Infinite Health, Infinite Money, Infinite Lives. These write into your save at area transitions; turning a cheat off doesn't undo what it already did.
- **Restart Game** — restart to the title screen or back to this app's launcher, without closing the app.
- **Higher internal resolutions** — 3x/4x/6x/8x, beyond the console's native tiers.
- **GPU Driver** — see below.

### GPU Driver

<a id="gpu-driver"></a>

On Adreno devices, **Settings → GPU Driver** lets you load a different Vulkan driver instead of your device's own — useful when the system driver is what's broken (see the crash fix above). It's per-app: nothing on your device is modified, no root needed.

Import a driver `.adpkg` or `.so` through the file picker and restart when asked. [Mr. Purple's purple-turnip](https://github.com/MrPurple666/purple-turnip/releases) builds are the usual source for Adreno handhelds. If you're not sure where to start and you're on **Android 12**, [**T24**](https://github.com/MrPurple666/purple-turnip/releases#release-vturnip_mrpurple_T24-toasted.adpkg) is a reasonable first try — but treat it as a starting point, not a guarantee, since driver compatibility varies by chip. A driver that doesn't work costs you nothing but a restart: the app confirms a new driver is actually working before keeping it, and falls back to the system driver otherwise.

This tab appears regardless of GPU, but the available replacement drivers are Adreno/Qualcomm builds, so there's nothing useful to import on Mali or other hardware.

## Troubleshooting Details

<a id="troubleshooting-details"></a>

The quick fixes above cover the common cases. This section explains *why*, for anyone who wants it.

**Mali white screen (fixed in `1.0.3`).** The renderer used a Vulkan feature (dual-source blending) that no Mali GPU supports, causing a white screen with missing textures. `1.0.3` added a fallback chosen automatically from the GPU's reported capabilities — no configuration needed. On GPUs without that feature, N64 coverage-based effects (like anti-aliased edges) are approximated rather than exact; no visible difference was found in testing on Mali-G57/G77. If you see edge or transparency artifacts on Mali, please open an issue.

**Adreno launch crash (fixed in `1.0.4`).** Some Adreno devices (reported on a Motorola G60, reproduced on an Adreno 630) closed the app a few seconds into gameplay. This is a bug in the Qualcomm Vulkan driver itself, not the game — the same build runs fine under a third-party driver on the same hardware. Two independent workarounds, either is enough:
- **Graphics → Framebuffer Effects → Off** avoids the driver call that crashes. Works everywhere, no installation needed, at the cost of some visual effects.
- **Settings → GPU Driver** (above) runs the game on a different driver entirely. Adreno hardware only.

Both settings are reachable even on an affected device — the crash only happens once the game itself starts rendering, and this app's own menu (Start Game / Controls / Settings / Mods) comes up before that.

**GPU Driver build notes.** Tested against Mr. Purple's Turnip builds on an Adreno 630 running Android 12:

| Build | Driver version | Result |
|---|---|---|
| **T21** | `25.2.0-devel-Unified-1.4.318` | Works |
| **T24** | `26.0.0-T24-1.4.335` | Works — the build the game was actually played on |
| **T29** | `26.2.0-T29-1.4.354` | Does not load on Android 12 (needs `pthread_getaffinity_np`, likely Android 13+) |

The package's own `minApi` field doesn't flag the T29 case — all three declare the same value. That's one GPU on one Android version, so treat it as a starting point rather than a compatibility list; newer builds generally want newer Android. A driver that works at first and degrades later isn't detected automatically — switch back by hand if that happens. And if Android kills the app very early in a session, a driver you'd already confirmed may ask for confirmation again next launch.

**Controller buttons that do nothing.** Controllers are recognized through SDL's controller database, bundled with the app. If your pad is newer than your Android version knows about, Android hands it over as a generic HID device with some buttons in slots that have no defined meaning — those buttons do nothing in *any* app, and remapping can't reach them. `1.0.4` added an entry for the DualSense Edge on Android 12 and earlier (Circle/R1/Create/Options weren't registering). Android 13+ already handles that pad correctly. For any other pad with dead buttons, open an issue with the model and Android version.

## ROM and Storage

This app is not an emulator and doesn't include copyrighted game assets. On first launch, the ROM you select is verified and copied into the app's private storage — no manual folder setup or legacy storage permissions required. Saves and settings live in the same app-scoped location.

## Building

The Android app is a Gradle module under `android/` that drives the native CMake build:

- Android Studio (or the Gradle wrapper), **NDK 27.1.12297006**, **CMake 3.22.1**
- Native code builds for `arm64-v8a`
- Initialize submodules first: `git submodule update --init --recursive`
- **A supported ROM is required at build time.** `RecompiledFuncs/` and `RecompiledPatches/` are generated (not committed) by running the host recompiler + patches codegen against the ROM; the ROM's data never ships in the APK. A clean clone can't build without this step.
- A release keystore can be supplied via `keystore.properties` (repo root or `android/`)

See **[BUILDING.md](BUILDING.md)** for the full step-by-step build (host tooling, ROM codegen, and the APK build).

## Credits

- [Goemon 64: Recompiled](https://github.com/klorfmorf/Goemon64Recomp) contributors
- [@linkzenic](https://github.com/linkzenic) — [Zelda64Recomp-Android](https://github.com/linkzenic/Zelda64Recomp-Android), whose Android port paved the way for this one
- [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp) contributors
- [RT64](https://github.com/rt64/rt64) contributors
- [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp), the base the upstream project builds on
- SDL contributors
- Icon and background graphic by [Jingleboy of Goemon International](https://goemoninternational.com)
