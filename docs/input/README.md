# Input assignment — reading `n64-goemon-input-assignment.csv`

`n64-goemon-input-assignment.csv` is the authored design source for the default
controller map (`recomp::default_n64_controller_mappings` in
`src/game/input.cpp`). It accumulated some draft/scratch material over time, so
read it with this legend (D5 review note):

## Authoritative columns — these match the shipped defaults
- **A — `Retroid Controls`**: the physical input.
- **B — `N64 Equivalent - Standard`**: the N64 button in Standard camera mode.
- **C — `N64 Equivalent - Analog Camera`**: the N64 button in Analog Camera mode
  (the right stick becomes the camera; D-pad covers the C-buttons).
- **G — `N64 input`**: the canonical N64 input name.

Verify any change against `default_n64_controller_mappings` and the CSV columns
A–C/G together; they agree with the code.

## Stale / do-NOT-trust material (kept only for history)
- **Column H — `Keyboard`**: a draft keyboard layout that **disagrees with the
  code**. The shipped keyboard defaults live in `default_keyboard_mappings`
  (`src/game/input.cpp`); the CSV keyboard column is not maintained.
- **Columns I–J — `Gamepad - Built-in Camera` / `Gamepad - Analog Camera`**: an
  **abandoned draft** layout (some secondary bindings are rotated one C-button
  off). Not reflected in code.
- **The lower mini-tables (rows after `Select`)**: scratch fragments from an
  earlier pass — duplicated, partial, and superseded by the rows above. Ignore.

If you re-author the CSV, strip columns H–J and the lower fragments; until then,
this note is the "mark" that keeps them from misleading a reader.
