# Goemon 64 Recompiled — Android

This fork packages [Goemon 64: Recompiled](https://github.com/klorfmorf/Goemon64Recomp) for Android. It is an early beta Android port of the recompilation, running the native game through [RT64](https://github.com/rt64/rt64) Vulkan rendering with SDL input, in-APK asset installation, and app-scoped ROM storage.

## Beta Notes

- This is an **early Android beta** (`0.2.0-dev`) and may still have device-specific Vulkan or driver issues.
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

## ROM and Storage

The app is not an emulator and does not include copyrighted game assets. On first launch the launcher screen asks you to select a supported ROM through the Android file picker; the ROM is verified and copied into the app's private storage, so no manual folder setup or legacy storage permissions are required. Saves and configuration are kept in the same app-scoped location.

## Building

The Android app is a Gradle module under `android/` that drives the native CMake build:

- Android Studio (or the Gradle wrapper) with **NDK 27.1.12297006** and **CMake 3.22.1**
- Native code builds with `-DGOEMON_ANDROID=ON` for `arm64-v8a`
- Initialize submodules first: `git submodule update --init --recursive`
- A release keystore can be supplied via `keystore.properties` (repo root or `android/`)

## Credits

- [Goemon 64: Recompiled](https://github.com/klorfmorf/Goemon64Recomp) contributors
- [@linkzenic](https://github.com/linkzenic) — [Zelda64Recomp-Android](https://github.com/linkzenic/Zelda64Recomp-Android), whose Android port paved the way for this one
- [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp) contributors
- [RT64](https://github.com/rt64/rt64) contributors
- [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp), the base the upstream project builds on
- SDL contributors
- Icon and background graphic by [Jingleboy of Goemon International](https://goemoninternational.com)
