# Goemon 64 Recompiled — Android

This fork packages [Goemon 64: Recompiled](https://github.com/klorfmorf/Goemon64Recomp) for Android. It is an Android port of the recompilation, running the native game through [RT64](https://github.com/rt64/rt64) Vulkan rendering with SDL input, in-APK asset installation, and app-scoped ROM storage.

## Notes

- This is the **first public release** (`1.0.0`); it may still have device-specific Vulkan or driver issues.
- Primarily developed and tested on **Retroid Pocket 5** and **AYN Thor** class handhelds (Snapdragon / Adreno, Android 13).
- Requires a working **Vulkan** driver. On devices with incomplete drivers, a custom Turnip driver (e.g. [Mr Purple](https://github.com/MrPurple666/purple-turnip/releases)) may help.
- `arm64-v8a` only for now. Other ABIs (including x86_64 for emulators) are not built yet.

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

This port has been tested primarily on Snapdragon / Adreno handhelds. If graphics are incorrect, crashes happen at game start, or Vulkan device creation fails, your device may need a newer or different Vulkan driver.

## What This Android Fork Adds

- Android APK packaging for Goemon 64: Recompiled (arm64-v8a)
- RT64 Vulkan rendering on Android
- ROM selection through the Android file picker (Storage Access Framework), copied into app-scoped storage
- Game/UI assets bundled in the APK and installed to private storage on first launch
- Physical controller support through SDL
- Landscape-locked, fullscreen game presentation

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
- Fullscreen (immersive) presentation is applied reliably on launch and re-applied when the app regains focus.
- The desktop-only Window Mode option is hidden on Android, and menu navigation skips it.

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
