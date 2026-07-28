#ifndef SCREEN_CAPTURE_PNG_IO_H
#define SCREEN_CAPTURE_PNG_IO_H

#include <X11/Xlib.h>
#include <vector>

// Encodes the given XImage as a PNG byte stream. Returns an empty vector on
// failure. Used by both saveScreenshot (file output) and copyToClipboard
// (in-memory handoff to the X11 clipboard handler).
std::vector<unsigned char> encodePNG(XImage* img, int width, int height);

#endif // SCREEN_CAPTURE_PNG_IO_H
