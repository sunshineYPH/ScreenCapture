#ifndef SCREEN_CAPTURE_PIN_WINDOW_H
#define SCREEN_CAPTURE_PIN_WINDOW_H

#include "renderer.h"

#include <X11/Xlib.h>

// Renders the selection rectangle to a Pixmap, writes it as a P6 PPM to
// /tmp/screen_capture_pin_<pid>_<n>.ppm, and fork+exec's this same binary
// with --pin <ppm_path> to spawn an independent pin window. The parent
// returns immediately and continues its normal flow (clipboard / exit).
bool pinToScreen(Display* display, Renderer* renderer,
                 int selX, int selY, int selW, int selH);

// Entry point for the spawned pin child process. Opens the X display, loads
// the PPM, draws a 2px accent border + 16x16 close button, and runs an
// event loop that supports drag (any mouse press outside the close button)
// and close (X button or Esc). Returns the process exit code.
int runPinWindow(const char* ppmPath);

#endif // SCREEN_CAPTURE_PIN_WINDOW_H
