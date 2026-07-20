---
name: Bug report
about: Report a bug in the Android app
title: ''
labels: ''
assignees: ''

---

## Do not report issues with mods on this page. Please report them on the repo for the mods themselves.

## If the app crashes on startup, this is most often a graphics-driver / Vulkan issue on your device's GPU. Please make sure your device is on the latest available system update before submitting.

**Does the crash happen on launch, when loading a ROM, or during gameplay?** Non-Adreno GPUs (e.g. Mali) are a known source of Vulkan/driver issues on boot.

**Have you checked whether this issue is vanilla behavior? In other words, does it occur on original hardware?**

**Were you playing with intended mechanics, or using glitches? If it's the latter, which glitches?**

**Describe the bug**
A clear and concise description of what the bug is.

**To Reproduce**
Steps to reproduce the behavior:
1. Go to '...'
2. etc.

**Expected behavior**
A clear and concise description of what you expected to happen.

**Screenshots**
Please attach a screenshot of the bug.

**Device information (please complete the following):**
 - Device model: [e.g. Retroid Pocket 5, AYN Thor, generic phone/tablet]
 - Android version: [e.g. Android 13]
 - SoC / GPU: [e.g. Snapdragon 865 / Adreno 650, Dimensity 1200 / Mali-G77]
 - App version: [e.g. 1.0.0 — shown on the launcher screen]
 - Install source: [GitHub release APK, or built from source]

**Logcat (if it crashes)**
If you can capture it, attach `adb logcat` output around the crash (tag `Goemon64-stdio`). This is the single most useful thing for diagnosing driver crashes.

**Mods Installed**
List the mods you had installed and enabled when you encountered the error.

**Additional context**
Add any other context about the problem here.
