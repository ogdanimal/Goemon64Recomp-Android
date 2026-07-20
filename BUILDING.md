# Building Guide

This guide covers building the **Android** app from source. The shipped product
is an Android APK (arm64-v8a); there is no supported desktop build in this fork.

Building requires you to provide a **decompressed** ROM of the US version of the
game — it is a build-time input only and its data is never shipped in the APK.

The steps below: clone with submodules, install the toolchain, decompress the
ROM, run the recompiler to generate C sources, then build the APK with Gradle.

## 1. Clone the repository (with submodules)

This project uses submodules, so clone recursively.

```bash
git clone --recurse-submodules https://github.com/ogdanimal/Goemon64Recomp-Android.git
cd Goemon64Recomp-Android
# if you forgot --recurse-submodules:
# git submodule update --init --recursive
```

## 2. Install dependencies

You need a host toolchain (to build and run the recompiler) plus the Android SDK.

### Host toolchain (Linux / WSL)

```bash
sudo apt-get install -y ninja-build cmake clang lld llvm make
```

### Android SDK

Install via Android Studio or the command-line SDK tools, then install the
pinned NDK and CMake versions the native build expects:

- **NDK 27.1.12297006**
- **CMake 3.22.1**
- **JDK 17**

```bash
sdkmanager "ndk;27.1.12297006" "cmake;3.22.1"
```

## 3. Decompress the target ROM

You need a decompressed NTSC-U *Mystical Ninja Starring Goemon* ROM
(decompressed ROM sha1: `df8083a54296b8c151917c5333e1c85f014a2a66`).

Follow the build instructions for the
[Mystical Ninja Starring Goemon Decompilation Project](https://github.com/klorfmorf/mnsg)
to generate the decompressed ROM, then copy it to the **repository root** and
name it **`mnsg.z64`** (this is the path `mnsg.toml` and `aspMain.toml` expect).

## 4. Generate the C sources (host)

Build [N64Recomp](https://github.com/Mr-Wiseguy/N64Recomp)
(building instructions [here](https://github.com/Mr-Wiseguy/N64Recomp?tab=readme-ov-file#building)).
That produces the `N64Recomp` and `RSPRecomp` executables; copy both to the
repository root.

You also need the `file_to_c` host tool, built from the bundled rt64 source:

```bash
mkdir -p build-host-tools
clang++ -std=c++17 -O2 lib/rt64/src/tools/file_to_c/file_to_c.cpp -o build-host-tools/file_to_c
```

Then, from the repository root, recompile the game, the RSP microcode, and the
mod patches:

```bash
# Game + RSP microcode  ->  RecompiledFuncs/*.c , rsp/aspMain.cpp
./N64Recomp mnsg.toml
./RSPRecomp aspMain.toml

# Patches  ->  patches/patches.bin + RecompiledPatches/*
make -C patches CC=clang LD=ld.lld
./N64Recomp patches.toml
./build-host-tools/file_to_c \
  patches/patches.bin mm_patches_bin \
  RecompiledPatches/patches_bin.c RecompiledPatches/patches_bin.h
```

> [!NOTE]
> `RecompiledFuncs/` and `RecompiledPatches/` are **generated**, not committed.
> The Android CMake build does not run the patches codegen for you, so this step
> must be done before building the APK.

## 5. Build the APK

The Android app is a Gradle module under `android/` that drives the native CMake
build (`-DGOEMON_ANDROID=ON`, `arm64-v8a`).

```bash
cd android
./gradlew assembleDebug --no-daemon --stacktrace
```

The debug APK lands at `android/app/build/outputs/apk/debug/app-debug.apk`.

To build a **release** APK, supply a signing keystore via `keystore.properties`
(at the repo root or in `android/`) and run `./gradlew assembleRelease`. Note
that a release signed with a different key will not install over an existing
install. (CI cuts signed releases automatically on `v*` tags.)

## 6. Success

Sideload the debug APK onto an arm64 Android device. It is not an emulator and
ships no game assets — on first launch it asks you to select your own ROM
through the Android file picker.

> [!IMPORTANT]
> In the app you provide a **standard** ROM through the file picker, not the
> decompressed one. The decompressed ROM is only a build-time input for step 3.
