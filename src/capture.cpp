#include "capture.h"
#include <X11/Xutil.h>
#include <sys/stat.h>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <unistd.h>
#include <pwd.h>

ScreenCapture::ScreenCapture(Display* display)
    : m_display(display)
    , m_image(nullptr)
    , m_width(0)
    , m_height(0)
{
    int screen = DefaultScreen(display);
    m_visual = DefaultVisual(display, screen);
    m_depth = DefaultDepth(display, screen);
}

ScreenCapture::~ScreenCapture() {
    if (m_image) {
        XDestroyImage(m_image);
        m_image = nullptr;
    }
}

bool ScreenCapture::captureFullScreen() {
    int screen = DefaultScreen(m_display);
    Window root = RootWindow(m_display, screen);

    m_width = DisplayWidth(m_display, screen);
    m_height = DisplayHeight(m_display, screen);

    // Destroy previous image if exists
    if (m_image) {
        XDestroyImage(m_image);
        m_image = nullptr;
    }

    // Capture full screen using XGetImage
    m_image = XGetImage(m_display, root, 0, 0, m_width, m_height, AllPlanes, ZPixmap);

    return (m_image != nullptr);
}

PixelColor ScreenCapture::getPixelColor(int x, int y) const {
    PixelColor color = {0, 0, 0};

    if (!m_image || x < 0 || y < 0 || x >= m_width || y >= m_height) {
        return color;
    }

    unsigned long pixel = XGetPixel(m_image, x, y);

    // Handle different bit depths
    if (m_depth == 24 || m_depth == 32) {
        color.r = (pixel >> 16) & 0xFF;
        color.g = (pixel >> 8) & 0xFF;
        color.b = pixel & 0xFF;
    } else if (m_depth == 16) {
        // 16-bit color: RGB565 or RGB555 depending on hardware
        color.r = ((pixel >> 8) & 0xF8);
        color.g = ((pixel >> 3) & 0xFC);
        color.b = ((pixel << 3) & 0xF8);
        // Scale to 0-255
        color.r |= (color.r >> 5);
        color.g |= (color.g >> 6);
        color.b |= (color.b >> 5);
    }

    return color;
}

std::string ScreenCapture::getScreenshotDir() {
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
        else home = "/tmp";
    }
    return std::string(home) + "/Pictures/Screenshots";
}

std::string ScreenCapture::generateFilename() {
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);

    std::ostringstream oss;
    oss << std::setfill('0')
        << (tm_info->tm_year + 1900)
        << std::setw(2) << (tm_info->tm_mon + 1)
        << std::setw(2) << tm_info->tm_mday << "_"
        << std::setw(2) << tm_info->tm_hour
        << std::setw(2) << tm_info->tm_min
        << std::setw(2) << tm_info->tm_sec
        << ".png";
    return oss.str();
}

Pixmap ScreenCapture::createPixmap(Display* display, Window window) {
    if (!m_image) return None;

    int screen = DefaultScreen(display);
    Pixmap pixmap = XCreatePixmap(display, window, m_width, m_height, DefaultDepth(display, screen));

    GC gc = XCreateGC(display, pixmap, 0, nullptr);
    XPutImage(display, pixmap, gc, m_image, 0, 0, 0, 0, m_width, m_height);
    XFreeGC(display, gc);

    return pixmap;
}