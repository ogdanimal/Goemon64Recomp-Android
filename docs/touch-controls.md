# On-screen (touch) controls

The app shipped controller-only. This adds a touchscreen control scheme so a plain
phone can play, without changing anything for a handheld that already has sticks.

- **What it looks like:** analog stick under the left thumb; A and B under the right
  with the C diamond above them; L, Z and R along the top edge under the index
  fingers; Start and a settings handle in the middle.
- **When it appears:** set under **Settings → Touch → On-Screen Controls**. Auto
  (the default) shows it until a gamepad is used, then hides it until the screen is
  touched again; On and Off are also available.
- **Settings:** the Touch tab also has **Stick Sensitivity** and **Edit Layout**.
  Long-press the ☰ handle for size, opacity and vibration. A short tap on ☰ opens
  the game's own menu (it is the on-screen stand-in for Select).
- **Reaching the tab:** Touch is the last tab, and with the GPU Driver tab present
  the row is wider than the menu, so it scrolls sideways. Drag the row with a
  finger; a controller scrolls it automatically as focus moves. RmlUi 6.0 has no
  drag-scrolling of its own, so this is `TabStripDragScroller` in
  `src/ui/ui_config.cpp`; a drag swallows the click that ends it, so dragging never
  switches tabs.

---

## How it works

### The overlay is an Android View, not part of the renderer

The pad is a transparent `View` composited over SDL's `SurfaceView`, drawn with
`Canvas`. The recomp has its own RmlUi layer and drawing the pad there was the obvious
alternative; a View wins on the things that matter here. It needs no knowledge of RT64
or Vulkan, so changing it never touches the native build, and it gets Android's
multi-touch, haptics and safe-area insets for free.

Everything is drawn as vectors, no bitmaps: the APK does not grow, it stays sharp at
any density, and restyling is an edit to one file rather than an image pipeline.

### It presents itself as an extra gamepad

The one seam into native code is a virtual gamepad shaped exactly like an SDL game
controller — a button bitmask and an axis array. `input.cpp` merges it inside
`controller_button_state()` and `controller_axis_state()`, the same two functions
every physical pad already flows through.

That single decision is what keeps the feature small:

- Every binding in **Settings → Controls** applies to the on-screen buttons unchanged.
  Rebind B and the on-screen B moves with it.
- A rebind can never detach it, because it has no bindings of its own.
- It **merges** with a physical pad rather than fighting it, exactly as two physical
  pads already do.
- Analog-camera mode, C-button masking, the autosave combo and mods need no knowledge
  that it exists.

The alternative — emitting N64 buttons directly — would have needed a second, parallel
binding system that could silently drift out of step with the real one.

### What each control emits

| On-screen | SDL input | N64 | In game |
|---|---|---|---|
| Stick | `AXIS_LEFTX` / `LEFTY` | Analog stick | Move |
| A | `BUTTON_A` | A | Jump |
| B | `BUTTON_B` | B | Attack |
| C▲ ▶ ▼ ◀ | `BUTTON_DPAD_UP/RIGHT/DOWN/LEFT` | C-buttons | Magic / Map / Character / Weapon |
| Z | `AXIS_TRIGGERLEFT` | Z | Crouch |
| R | `AXIS_TRIGGERRIGHT` | R | Camera / Hook Chain |
| L | `BUTTON_LEFTSHOULDER` | L | Unused by the game; for mods |
| Start | `BUTTON_START` | Start | Pause |
| ☰ | `BUTTON_BACK` | — | Opens the recomp's settings menu |

**Why the C-buttons emit D-pad.** Each C direction has three stock bindings: a
face/shoulder button, the right stick, and the D-pad. The D-pad is the only one that
covers all four directions (C-Right has no face button at all) *and* survives
analog-camera mode, which suppresses the right stick. So D-pad is the binding that
makes the on-screen C cluster behave the same either way.

### Details that matter in the hand

- **Hit targets are bigger than the artwork.** A thumb's contact patch is wide and its
  reported centre lands low of where the player thinks they pressed.
- **Hit-testing runs smallest control first**, so a deliberate press on a small C
  button is never swallowed by the stick's generous margin.
- **State, not edges.** Every event recomputes the whole button mask from the live
  pointers, rather than keeping per-button hold counters. Two fingers on one button
  work either way, but only this version has no counter that can drift, so a gesture
  cancelled by the system cannot strand a button held forever.
- **The overlay owns the whole gesture** while it is shown. Android delivers every
  pointer of a gesture to whichever view claimed its `ACTION_DOWN`; letting an
  empty-space touch fall through to SDL would hand SDL the rest of the gesture, and a
  thumb resting on the picture would silently kill the buttons under the other hand.
- **The ☰ handle is click-on-release**, alone among the controls. A tap opens the
  game's menu and a long press opens the overlay's settings, and those are only
  distinguishable once the finger lifts. Every other control fires on contact.
- **The pad hides for menus**, polled from `recompui::is_context_capturing_input()`,
  and stops consuming touches entirely so SDL's touch-to-mouse emulation can drive the
  RmlUi menu underneath.
- **Sizes scale off `TouchLayout.sizingUnit()`** — the height of the widest 16:9 box
  that fits. Sizing off height alone oversizes buttons on a 4:3 screen until they
  collide; off width alone they balloon on a 21:9.

---

## Where the code is

| File | What it does |
|---|---|
| `touch/TouchControl.java` | The control vocabulary and the SDL input each one emits |
| `touch/TouchLayout.java` | Geometry, sizing, safe-area clamping, persistence |
| `touch/TouchPad.java` | Multi-touch state machine → button mask + axes |
| `touch/TouchOverlayView.java` | Drawing and touch dispatch |
| `touch/TouchOverlayController.java` | Installs the view, polls menu state, settings + editor |
| `touch/TouchPrefs.java` | SharedPreferences persistence |
| `touch/NativeTouch.java` | The JNI seam |
| `src/main/android_touch.cpp` | Virtual pad state + JNI entry points |
| `include/goemon_touch.h` | The API `input.cpp` reads |
| `src/game/input.cpp` | The merge into the physical-pad path |
| `src/ui/ui_state.cpp` | Publishes the per-frame menu-open snapshot the overlay polls (`is_context_capturing_input_snapshot`), so the poll never waits on `ui_state_mutex` |
| `src/ui/ui_config.cpp` | The Touch tab's bindings, and `TabStripDragScroller` for the scrolling tab row |

### The SDL constants are duplicated

JNI cannot read a C enum, so `TouchControl.Sdl` holds a copy of the SDL button and axis
values. `android_touch.cpp` static-asserts every one against the real enum, and the
failure message names `TouchControl.java`. If SDL is ever bumped and renumbers, the
native build breaks loudly instead of shipping a pad where every button quietly presses
the wrong thing.

---

## Known gaps

- **No layout profiles.** One layout, not a set you can switch between. The editor and
  the serialisation format would both take it, but nothing selects among them yet.
- **The stick has no floating mode.** The base is fixed. A "recentre where the thumb
  lands" option is a common preference and is not implemented.
- **The editor moves and resizes controls, but does not rotate or reshape them.**
- **The labels assume the default bindings.** The on-screen "A" sends the controller's
  A button, not N64 A, so it means whatever **Settings → Controls** has bound to that
  button. With the stock bindings every label is right; after a remap, the labels can
  be wrong.
- **There is no right stick.** Analog Camera mode is driven by the right stick, and
  its recentre by R3, so neither can be used from the touchscreen alone. The C
  diamond still works in that mode, because it emits D-pad rather than right-stick
  input.
- **A very short tap can be missed.** The overlay reports what is held right now and
  the game samples it once per input poll, so a press that starts and ends between
  two polls is never seen. A finger is normally down long enough; a synthetic
  `adb shell input tap` is not. Holding each press for a minimum time would close
  this. (Inferred from the design and seen with `adb`, not seen with a finger.)
- **No per-orientation layouts.** The app is landscape-locked
  (`android:screenOrientation="landscape"`), so there is only one to store. If that
  lock is ever lifted, layouts would need storing per orientation.
