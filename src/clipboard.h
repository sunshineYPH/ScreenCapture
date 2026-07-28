#ifndef SCREEN_CAPTURE_CLIPBOARD_H
#define SCREEN_CAPTURE_CLIPBOARD_H

#include "capture.h"
#include "renderer.h"

#include <X11/Xlib.h>
#include <string>
#include <vector>

// State shared between the main event loop and the X11 clipboard handler.
// Set by copyToClipboard(), read by the SelectionRequest/SelectionClear
// dispatch in main().
extern bool g_clipboardActive;
extern std::vector<unsigned char> g_clipboardData;
extern Window g_clipboardWin;
extern Atom g_clipboardPngAtom;
extern int g_clipboardTimeout;
extern Time g_clipboardTimestamp;

// Encodes the given selection rectangle as PNG and registers it as the
// CLIPBOARD selection owner via an override-redirect offscreen window.
// The main loop's select()-based event pump then services
// SelectionRequest/SelectionClear events for up to g_clipboardTimeout
// 100ms ticks.
bool copyToClipboard(Display* display, ScreenCapture* capture, Renderer* renderer,
                     int selX, int selY, int selW, int selH);

// Encodes the given selection rectangle as PNG and writes it to
// ~/Pictures/Screenshots/ with a YYYYMMDD_hhmmss.png filename. Returns
// false if the directory can't be created or the file can't be opened.
bool saveScreenshot(Display* display, ScreenCapture* capture, Renderer* renderer,
                    int selX, int selY, int selW, int selH);

#endif // SCREEN_CAPTURE_CLIPBOARD_H
