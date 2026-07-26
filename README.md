# Goemon 64 Recompiled — Android

This fork packages [Goemon 64: Recompiled](https://github.com/klorfmorf/Goemon64Recomp) for Android. It is an Android port of the recompilation, running the native game through [RT64](https://github.com/rt64/rt64) Vulkan rendering with SDL input, in-APK asset installation, and app-scoped ROM storage.

## Notes

- Primarily developed and tested on **Retroid Pocket 5** and **AYN Thor** class handhelds (Snapdragon / Adreno). Device-specific Vulkan or driver issues are still possible on untested hardware.
- **Mali GPUs are supported as of `1.0.3`.** Earlier versions rendered a white screen with missing textures on Mali devices; see the Mali note below.
- **Some older Adreno devices crashed on launch before `1.0.4`.** If the game closes itself a few seconds after starting, see the Adreno note below.
- Requires a working **Vulkan** driver. If your device's driver is the problem, `1.0.4` can load a driver you supply yourself — see **Settings → GPU Driver**, and the Adreno note below.

### Mali GPUs

Versions before `1.0.3` showed a white screen with most textures missing on devices with Mali GPUs, because the renderer required a Vulkan feature (dual-source blending) that no Mali GPU supports. `1.0.3` adds a fallback that is selected automatically on any GPU without that feature, so no configuration is needed.

The fix was developed on a Mali-G57 device and confirmed by the original reporter on a Mali-G77 (Retroid Pocket 4 Pro). It is chosen from the GPU's reported capabilities rather than from a device list, so it should engage on any Mali GPU. On GPUs without dual-source blending, N64 coverage-based effects such as anti-aliased edges are approximated rather than exact; no visible difference was found in testing. If you see edge or transparency artifacts on a Mali device, please open an issue.

### Adreno launch crash

Some Adreno devices closed the app a few seconds after starting the game, before it reached the title screen. This was reported on a Motorola G60 and reproduced here on an Adreno 630. It is a bug in the Qualcomm Vulkan driver on those devices, not in the game: the same build runs on the same hardware under a third-party driver, and crashes under the system one.

`1.0.4` adds two independent ways around it, either of which is enough on its own:

- **Graphics → Framebuffer Effects → Off** avoids the driver call that crashes. This works on any device and needs nothing installed, at the cost of some visual effects (see the Features section below).
- **Settings → GPU Driver** lets you install a different Vulkan driver and run the game on that instead. The Mesa Turnip builds by **Mr. Purple** ([purple-turnip](https://github.com/MrPurple666/purple-turnip/releases)) are the ones widely used on retro handhelds, and are what this was developed against. Adreno hardware only.

Both are reachable even on an affected device: the crash happens once the game itself starts rendering, while the app's own menu screen (Start Game / Controls / Settings / Mods) comes up before that and is unaffected. Change the setting there, then start the game.

This fork is based on Goemon 64: Recompiled:

https://github.com/klorfmorf/Goemon64Recomp

Goemon 64: Recompiled uses [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp) to statically recompile *Mystical Ninja Starring Goemon* into a native port, with [RT64](https://github.com/rt64/rt64) as the rendering engine. For general project information, features, and desktop releases, see the upstream repository.

## Download

Android builds are published on this repo's releases page:

https://github.com/ogdanimal/Goemon64Recomp-Android/releases

The release APK does not contain the game ROM. You must provide your own legally obtained copy when the app asks for it.

## Android Requirements

- Android 9.0 (API 28) or newer
- ARM64 (`arm64-v8a`) device
- Vulkan-capable GPU and a working Vulkan driver (tested on Snapdragon / Adreno handhelds)
- Enough free storage for the app data folder, the imported ROM, and saves

This port has been tested primarily on Snapdragon / Adreno handhelds, and on Mali-G57 and Mali-G77 devices (see the Mali note above). If graphics are incorrect, crashes happen at game start, or Vulkan device creation fails, your device may need a newer or different Vulkan driver — on Adreno hardware you can supply one yourself under **Settings → GPU Driver**.

## What This Android Fork Adds

- Android APK packaging for Goemon 64: Recompiled (arm64-v8a)
- RT64 Vulkan rendering on Android
- ROM selection through the Android file picker (Storage Access Framework), copied into app-scoped storage
- Game/UI assets bundled in the APK and installed to private storage on first launch
- Physical controller support through SDL
- Landscape-locked, fullscreen game presentation
- Optional loading of a user-supplied Vulkan driver on Adreno devices

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

The right stick and the D-Pad both cover the C-buttons, so with Analog Camera on the right stick orbits while the D-Pad keeps the C-button actions.

### Controller compatibility

Controllers are recognised through SDL's own controller database, which ships with the app. If a pad is newer than your Android version, Android may not know its layout and will hand it over as a generic HID device, with some buttons landing in slots that have no meaning — those buttons then do nothing, in every app, and remapping cannot reach them.

`1.0.4` adds a database entry for the **DualSense Edge on Android 12 and earlier**, where Circle, R1, Create and Options did not register. Devices running Android 13 or newer already enumerate the pad correctly and are unaffected. If your controller has buttons that do nothing and do not appear when rebinding, please open an issue with the pad's model and your Android version.

## Features

Beyond the Android packaging above, this fork adds the following. Everything here is optional and off by default unless noted, and lives in the in-game settings menu.

### Analog Camera

A free-look camera on the right analog stick, which the original game does not have.

- **Analog Camera** (General) — orbit the camera around Goemon with the right stick. The camera holds the angle you set; area transitions and scripted camera moves still take over as normal.
- **Zoom** — hold **R** (the right trigger) and push the right stick up or down to zoom the camera in and out.
- **Analog Camera Invert** — per-axis inversion (None / X / Y / Both). Also flips the zoom direction.
- **Camera Sensitivity X / Y** — 0–100 per axis, 50 being the tuned default rate. Zero disables that axis.
- Click the right stick (**R3**) to hand the camera back to the game so it resumes following behind you.

While enabled, the right stick's C-button mapping is silenced so it can drive the camera, and **R** becomes the zoom modifier instead of the game's native camera control.

### Swap Characters While Moving

- **Swap Characters While Moving** (General) — lets you start a character swap (C-Down) while walking or running, instead of only from a standstill. With it on, C-Down also swaps once per press rather than repeating while held.

Note this does not make swapping faster. Changing character reloads the new character's model from the game data, which costs about a second of locked input either way; this only removes the need to stop first. Swapping stays blocked in states where it would corrupt the game — notably on ladders, and while a swap is already in progress.

### Attack While Moving

- **Attack While Moving** (General) — lets you keep moving during a ground attack instead of rooting in place. You lunge in the direction you were last running. Covers each character's melee combos, throws, bombs, and specials, including the upgraded weapons.

This is a novelty toggle: the attack animation stays planted while you glide along, so the character appears to slide during the swing. Normal movement resumes the moment the attack ends. Goemon's chain pipe is deliberately unaffected, since it anchors to a fixed point in the world.

### Autosave

- **Autosave** (General) — saves your progress using the game's own save system, so an autosave is an ordinary save: loading one starts you where a save made at that point normally would. It writes to the save slot you loaded.

While it is on, press **L + R + Z** during normal gameplay to save immediately.

Saving is refused unless the game is in normal gameplay — the check reuses the game's own "can the player open the pause menu right now" conditions, so cutscenes, dialogue, the pause menu, area transitions and loading are all excluded, as are the Impact and sidescroller stages.

**This overwrites your existing save file. Back it up before enabling it.**

### Cheats

A **Cheats** tab, all options off by default:

- **Infinite Health** — refills the life meter every frame. Does not protect against instant-death hazards such as pits.
- **Infinite Money** — holds your ryo at 9999, the game's own maximum. Shops still deduct; the total refills immediately after.
- **Infinite Lives** — keeps remaining lives at 3. Dying still costs the life and returns you to a checkpoint; the counter is restored.

Cheats change live game state, and that state is written to your save file at area transitions. Turning a cheat off stops it acting but does not undo it — whatever it raised stays raised in your save.

### Restart Game

A restart button (the circular arrow) sits next to the exit and close icons at the top right of the settings menu. It offers two destinations:

- **To Title Screen** — restarts the game from a cold boot, as if you had just launched it.
- **To App Menu** — returns to this app's own launcher screen (Start Game / Controls / Settings / Mods), without closing the app.

Either way, anything since your last save is lost, so the prompt asks for confirmation. Both take a few seconds, as the game is genuinely reloaded rather than rewound. The button only appears once a game is running.

### Display and Presentation

- **Higher internal resolutions** — Original 3x / 4x / 6x / 8x in addition to the stock tiers. Downsampling is only offered at Original and Original 2x, where it is meaningful.
- **Framebuffer Effects** (Graphics, default **On**) — keeps the console's framebuffer in memory and the image on the GPU in sync. Effects that read back or draw straight into the rendered image depend on it, so leave it on unless the game will not launch on your device; see the Adreno note above.
- Fullscreen (immersive) presentation is applied reliably on launch and re-applied when the app regains focus.
- The desktop-only Window Mode option is hidden on Android, and menu navigation skips it.

### GPU Driver

On Adreno devices, **Settings → GPU Driver** can run the game on a Vulkan driver you supply instead of your device's own — useful when the system driver is the thing that is broken. It is a per-app choice: nothing about your device is modified, no root is needed, and no other app is affected.

Import a driver `.adpkg` package or a bare `.so` through the file picker, select it, and restart the app when asked. Mr. Purple's [purple-turnip](https://github.com/MrPurple666/purple-turnip/releases) releases are the usual source on Adreno handhelds, and are what the feature was tested with; other Turnip builds packaged the same way should work too. Import rejects anything that is not a 64-bit Arm shared library, and any package missing the library its `meta.json` names, telling you which. Whether the file is genuinely a Vulkan driver is only known when it loads — if it is not, the game says so and carries on with the system driver.

**Which build to start with.** These are Mr. Purple's Turnip builds, as tested on the Adreno 630 device this was developed on, running Android 12:

| Build | Driver version | Result |
|---|---|---|
| **T21** | `25.2.0-devel-Unified-1.4.318` | Works |
| **T24** | `26.0.0-T24-1.4.335` | Works — the build the game was actually played on |
| **T29** | `26.2.0-T29-1.4.354` | **Does not load on Android 12.** Needs a C library function (`pthread_getaffinity_np`) that Android 12 does not provide; expected to need Android 13 or newer |

The package's own `minApi` field will not warn you about the T29 case — all three declare the same value. If a driver will not load the game tells you and falls back to the system driver, so trying another costs nothing.

That is one GPU on one Android version, so treat it as a place to start rather than a compatibility list. Newer builds generally want newer Android.

Because a bad driver can leave you with a black screen or a hang, a newly selected driver has to be **confirmed**: the game asks "Keep this graphics driver?" once it is running, and a driver that never gets that far is switched back to the system one on the next launch. So a driver that does not work costs you a restart, not your ability to reach the settings menu.

The system driver is always the default, and the tab is only present on Android. Note that it is not gated on your GPU: the tab appears on Mali and other hardware too, but the replacement drivers that exist are Adreno/Qualcomm builds, so there is nothing useful to import on those devices.

Two limits worth knowing. A driver that works at first and breaks later, while still producing frames, is not detected automatically — switch back by hand. And if Android kills the app very early in a session, a driver you had already confirmed may ask for confirmation again on the next launch.

## ROM and Storage

The app is not an emulator and does not include copyrighted game assets. On first launch the launcher screen asks you to select a supported ROM through the Android file picker; the ROM is verified and copied into the app's private storage, so no manual folder setup or legacy storage permissions are required. Saves and configuration are kept in the same app-scoped location.

## Building

The Android app is a Gradle module under `android/` that drives the native CMake build:

- Android Studio (or the Gradle wrapper) with **NDK 27.1.12297006** and **CMake 3.22.1**
- Native code builds for `arm64-v8a` (the NDK toolchain sets CMake's `ANDROID` flag)
- Initialize submodules first: `git submodule update --init --recursive`
- **A supported ROM is required at build time.** `RecompiledFuncs/` and
  `RecompiledPatches/` are generated (not committed) by running the host
  recompiler + patches codegen against the ROM; the ROM's data never ships in
  the APK. A clean clone cannot build without this step.
- A release keystore can be supplied via `keystore.properties` (repo root or `android/`)

See **[BUILDING.md](BUILDING.md)** for the full step-by-step build (host tooling,
ROM codegen, and the APK build).

## Credits

- [Goemon 64: Recompiled](https://github.com/klorfmorf/Goemon64Recomp) contributors
- [@linkzenic](https://github.com/linkzenic) — [Zelda64Recomp-Android](https://github.com/linkzenic/Zelda64Recomp-Android), whose Android port paved the way for this one
- [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp) contributors
- [RT64](https://github.com/rt64/rt64) contributors
- [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp), the base the upstream project builds on
- SDL contributors
- Icon and background graphic by [Jingleboy of Goemon International](https://goemoninternational.com)
