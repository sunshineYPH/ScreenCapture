#ifndef SCREEN_CAPTURE_BUTTON_ICONS_H
#define SCREEN_CAPTURE_BUTTON_ICONS_H

#include <X11/Xlib.h>
#include <string>
#include <vector>

// Loads the menu-icon PNGs from ./assets/ (or fallback paths) and
// caches them as 32-bit ARGB server-side Pixmaps keyed by button
// label.
//
// Drawing model: each icon is a 32-bit depth Pixmap carrying the
// full pre-multiplied alpha from the source PNG. The renderer
// composites it onto the screen-capture buffer using the XRender
// extension's PictOpOver blend mode — so a transparent PNG
// background blends correctly with whatever is underneath (the
// button's gray face) and the original figure color is preserved
// pixel-for-pixel.
//
// XRender is part of the X11 core protocol (every X server since
// X11R6.7 / 2003) but the *headers* (X11/extensions/Xrender.h) and
// the import library (libXrender) live in the libxrender-dev
// package. We require them at build time and fail to compile if
// they're missing — no runtime fallback, because none of the
// alternative X11 drawing paths can replicate the original PNG's
// color and transparency simultaneously.
namespace button_icons {

// Load and cache icons. Must be called once after the X display is
// open. Safe to call again — old Pixmaps are freed and replaced.
void init(Display* display, int screen,
          const std::string& assetsDir = "./assets",
          int targetSize = 24);

// Free all cached Pixmaps and the destination-Picture cache. Call
// before XCloseDisplay.
void cleanup();

// Look up an icon by its menu button label. Returns true and fills
// *pixmap, *w, *h with the cached 32-bit ARGB Pixmap for the named
// button if it was successfully loaded. Otherwise returns false so
// the caller can fall back to a text label.
bool get(const std::string& label, Pixmap* pixmap, int* w, int* h);

// Convenience helper: composite a previously-obtained icon Pixmap
// onto `dest` at (dstX, dstY). The icon's alpha is honored via
// XRender's PictOpOver blend — transparent PNG background blends
// with whatever is already in `dest`, fully-opaque figure pixels
// overwrite it.
void drawIcon(Display* display, Drawable dest, GC /*gc*/,
              Pixmap icon, int dstX, int dstY, int w, int h);

// Paint a colored overlay over the icon's figure pixels only,
// using the icon's own alpha channel as a mask. The overlay
// color (r, g, b) is blended at the requested alpha (0-255)
// over whatever is already at (dstX, dstY) on `dest`. Pixels
// with alpha = 0 in the source icon are left untouched, so the
// overlay only shows up on the figure.
//
// Used for hover/press highlighting: a white overlay at low
// alpha makes the icon visibly brighter without changing the
// icon's underlying hue or shape.
void drawIconHighlight(Display* display, Drawable dest, Pixmap icon,
                       int dstX, int dstY, int w, int h,
                       int r, int g, int b, int alpha);

std::vector<std::string> availableLabels();

} // namespace button_icons

#endif // SCREEN_CAPTURE_BUTTON_ICONS_H
