#ifndef SCREEN_CAPTURE_PNG_IO_H
#define SCREEN_CAPTURE_PNG_IO_H

#include <X11/Xlib.h>
#include <cstdint>
#include <string>
#include <vector>

// Encodes the given XImage as a PNG byte stream. Returns an empty vector on
// failure. Used by both saveScreenshot (file output) and copyToClipboard
// (in-memory handoff to the X11 clipboard handler).
std::vector<unsigned char> encodePNG(XImage* img, int width, int height);

// RGBA pixel data decoded from a PNG file. The data is tightly packed
// in row-major order, 4 bytes per pixel, top row first.
struct DecodedPNG {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // size = width * height * 4
};

// Decodes a PNG file from disk into RGBA pixels. Returns an empty
// DecodedPNG (width==0) on any failure (file missing, not a PNG,
// unsupported format, libpng error, etc.). Used to load the small
// menu-icon assets under assets/*.png.
//
// Note: This is intentionally read-only and minimal — the project's
// primary PNG path is encodePNG, which is hand-rolled. We only need
// a reader for fixed-format icon assets, so libpng is fine here.
DecodedPNG decodePNGFile(const std::string& path);

#endif // SCREEN_CAPTURE_PNG_IO_H
