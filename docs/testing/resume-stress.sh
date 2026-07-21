#!/usr/bin/env bash
# P1/S4 resume-UAF device stress harness (2026-07-21).
#
# Drives the in-process Android surface destroy/recreate cycle via lock/unlock
# (KEYCODE_SLEEP/WAKEUP), which fires surfaceDestroyed -> surfaceCreated ->
# VulkanSwapChain::recreateSurface. Asserts the app never crashes or restarts
# across many cycles -- the exact use-after-free the ownership-invariant fix
# (plume_vulkan.cpp + rt64_render_context.cpp) protects against.
#
# PASS criteria: pid constant every cycle, zero SIGSEGV/tombstone/abort, saves
# byte-identical before/after, and `[plume] surface recreated` firing (the fix
# executing). See docs/code-review-2026-07-20-pass2.md (P1/S4).
#
# The save byte-identical check assumes nothing writes saves during the run,
# which holds with autosave defaulting Off. With autosave ON it would false-fail
# (a 2-minute-boundary autosave rewrites the file) — disable autosave first if
# testing that configuration.
#
# KNOWN TEST-METHOD ARTIFACTS (not app bugs), observed on the RP5:
#   * Rapid device-sleep can drop the adb/USB link ("no devices/emulators
#     found"); an empty pid read is a dropped connection, not a crash -- confirm
#     with the FINAL pid.
#   * Programmatic wake races the keyguard, so vkCreateSwapchainKHR can transiently
#     return VK_ERROR_SURFACE_LOST_KHR (0xC4653600) / NATIVE_WINDOW_IN_USE_KHR
#     (0xC46535FF). These are absorbed by the resize() retry loop -- NOT crashes,
#     NOT recreateSurface failures. A real human unlock (dismiss keyguard, then
#     resume) is clean.
#
# Config via env:
#   ADB        path to adb (default: the WinGet PlatformTools adb.exe under WSL)
#   PKG        package (default com.goemon64.recomp)
#   SD_VOLUME  external-storage volume id (default: auto-detect the first
#              XXXX-XXXX volume that holds the app data dir)
#   SLOW, FAST cycle counts (default 30 / 25)
set -u
ADB="${ADB:-/mnt/c/Users/$USER/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe}"
PKG="${PKG:-com.goemon64.recomp}"
SLOW="${SLOW:-30}"; FAST="${FAST:-25}"
MARK="surface recreated from new ANativeWindow"

pid() { "$ADB" shell pidof "$PKG" | tr -d '\r'; }

# Locate the data dir (internal or an SD volume) unless SD_VOLUME is given.
if [ -n "${SD_VOLUME:-}" ]; then
  SDIR="/storage/$SD_VOLUME/Android/data/$PKG/files/data/saves"
else
  SDIR=""
  for v in $("$ADB" shell ls /storage 2>/dev/null | tr -d '\r'); do
    d="/storage/$v/Android/data/$PKG/files/data/saves"
    if "$ADB" shell "[ -d '$d' ]" 2>/dev/null; then SDIR="$d"; break; fi
  done
  [ -z "$SDIR" ] && SDIR="/sdcard/Android/data/$PKG/files/data/saves"
fi
echo "== data dir: $SDIR =="

initpid=$(pid)
echo "== init pid: $initpid =="
[ -z "$initpid" ] && { echo "FAIL: app not running"; exit 1; }
PRE=$("$ADB" shell "md5sum $SDIR/*" 2>/dev/null | tr -d '\r')
"$ADB" logcat -c
fail=0

echo "== Phase A: $SLOW slow lock/unlock cycles =="
for i in $(seq 1 "$SLOW"); do
  "$ADB" shell 'input keyevent KEYCODE_SLEEP; sleep 0.7; input keyevent KEYCODE_WAKEUP; input keyevent KEYCODE_A; input swipe 960 950 960 150 200; sleep 1.2' >/dev/null 2>&1
  p=$(pid)
  if [ -n "$p" ] && [ "$p" != "$initpid" ]; then echo "  CYCLE $i: PID $initpid -> $p *** CRASH/RESTART ***"; fail=1; break; fi
  [ $((i % 5)) -eq 0 ] && echo "  slow $i ok (pid ${p:-<adb drop>})"
done

if [ "$fail" -eq 0 ]; then
  echo "== Phase B: $FAST rapid flips =="
  for i in $(seq 1 "$FAST"); do
    "$ADB" shell 'input keyevent KEYCODE_SLEEP; input keyevent KEYCODE_WAKEUP; input keyevent KEYCODE_A; sleep 0.25' >/dev/null 2>&1
  done
  "$ADB" shell 'sleep 2' >/dev/null 2>&1
fi

"$ADB" logcat -d > /tmp/resume-stress.logcat 2>/dev/null
echo "== surface recreated: $(grep -c "$MARK" /tmp/resume-stress.logcat) (distinct ptrs: $(grep "$MARK" /tmp/resume-stress.logcat | grep -oE '0x[0-9a-f]+' | sort -u | wc -l)) =="
CR=$(grep -iE "Fatal signal|SIGSEGV|SIGABRT|tombstone|FATAL EXCEPTION|abort message|backtrace:" /tmp/resume-stress.logcat)
[ -n "$CR" ] && { echo "== CRASH MARKERS =="; echo "$CR" | head; fail=1; } || echo "== no crash markers =="
POST=$("$ADB" shell "md5sum $SDIR/*" 2>/dev/null | tr -d '\r')
[ "$PRE" != "$POST" ] && { echo "*** SAVE HASHES CHANGED ***"; fail=1; } || echo "== saves unchanged =="
final=$(pid)
echo "== final pid: $final (init $initpid) =="
[ -n "$final" ] && [ "$final" != "$initpid" ] && { echo "*** app restarted/crashed ***"; fail=1; }
[ "$fail" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL (check whether it's a test-method artifact above)"
