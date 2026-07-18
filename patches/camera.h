#ifndef PATCH_CAMERA_H
#define PATCH_CAMERA_H

// Per-frame analog (right-stick) camera update. Call once per frame from a
// patched per-frame function; folds right-stick input into the yaw offset and
// manages right-stick C-button suppression.
void update_analog_camera(void);

// Rotate the render camera's eye around its focus by the accumulated analog
// yaw, on a private copy. Returns the copy to feed to the view build
// (func_80017D8C_1898C / func_8001B6D4_1C2D4), or `cam` unchanged when the
// analog camera is inactive. Must be called BEFORE func_80017D8C_1898C — that
// is where guPerspective/guLookAtHilite consume eye and look_at.
Camera* apply_analog_camera(Camera* cam);

#endif // PATCH_CAMERA_H
