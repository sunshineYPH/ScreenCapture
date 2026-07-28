#ifndef CAPTURE_H
#define CAPTURE_H

#include <X11/Xlib.h>
#include <string>

struct PixelColor {
    int r, g, b;
};

class ScreenCapture {
public:
    ScreenCapture(Display* display);
    ~ScreenCapture();

    // Capture full screen
    bool captureFullScreen();

    // Get pixel color at position
    PixelColor getPixelColor(int x, int y) const;

    // Get captured data
    XImage* getImage() const { return m_image; }
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

    // Create pixmap from captured image (for double buffering)
    Pixmap createPixmap(Display* display, Window window);

    // Get screenshot directory path
    static std::string getScreenshotDir();
    static std::string generateFilename();

private:
    Display* m_display;
    XImage* m_image;
    int m_width;
    int m_height;
    Visual* m_visual;
    int m_depth;
};

#endif // CAPTURE_H