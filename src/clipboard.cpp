#include "clipboard.h"
#include "png_io.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <iostream>
#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>

bool g_clipboardActive = false;
std::vector<unsigned char> g_clipboardData;
Window g_clipboardWin = 0;
Atom g_clipboardPngAtom = 0;
int g_clipboardTimeout = 0;
Time g_clipboardTimestamp = 0;

bool saveScreenshot(Display* display, ScreenCapture* capture, Renderer* renderer,
                    int selX, int selY, int selW, int selH) {
    std::string dir = ScreenCapture::getScreenshotDir();
    struct stat st;
    if (stat(dir.c_str(), &st) != 0) {
        if (mkdir(dir.c_str(), 0755) != 0) {
            std::cerr << "Error: Cannot create directory " << dir << std::endl;
            return false;
        }
    }

    std::string filename = ScreenCapture::generateFilename();
    std::string filepath = dir + "/" + filename;

    if (!capture->getImage()) {
        std::cerr << "Error: No captured image" << std::endl;
        return false;
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    Pixmap pixmap = XCreatePixmap(display, root, selW, selH, 24);
    if (!pixmap) {
        std::cerr << "Error: Cannot create pixmap" << std::endl;
        return false;
    }

    renderer->renderToPixmap(pixmap, selX, selY, selW, selH);

    XImage* img = XGetImage(display, pixmap, 0, 0, selW, selH, AllPlanes, ZPixmap);
    if (!img) {
        std::cerr << "Error: Cannot get image from pixmap" << std::endl;
        XFreePixmap(display, pixmap);
        return false;
    }

    std::vector<unsigned char> pngData = encodePNG(img, selW, selH);
    XDestroyImage(img);
    XFreePixmap(display, pixmap);

    FILE* f = fopen(filepath.c_str(), "wb");
    if (!f) {
        std::cerr << "Error: Cannot create file " << filepath << std::endl;
        return false;
    }

    fwrite(pngData.data(), 1, pngData.size(), f);
    fclose(f);

    std::cout << "Screenshot saved: " << filepath << std::endl;
    return true;
}

bool copyToClipboard(Display* display, ScreenCapture* capture, Renderer* renderer,
                     int selX, int selY, int selW, int selH) {
    if (!capture->getImage()) {
        std::cerr << "Error: No captured image" << std::endl;
        return false;
    }

    Window root = RootWindow(display, DefaultScreen(display));
    Pixmap pixmap = XCreatePixmap(display, root, selW, selH, 24);
    if (!pixmap) {
        std::cerr << "Error: Cannot create pixmap" << std::endl;
        return false;
    }

    renderer->renderToPixmap(pixmap, selX, selY, selW, selH);
    XImage* img = XGetImage(display, pixmap, 0, 0, selW, selH, AllPlanes, ZPixmap);
    XFreePixmap(display, pixmap);

    if (!img) {
        std::cerr << "Error: Cannot get image from pixmap" << std::endl;
        return false;
    }

    g_clipboardData = encodePNG(img, selW, selH);
    XDestroyImage(img);

    g_clipboardPngAtom = XInternAtom(display, "image/png", False);

    g_clipboardWin = XCreateSimpleWindow(display, root, -100, -100, 1, 1, 0, 0, 0);
    if (!g_clipboardWin) {
        std::cerr << "Error: Cannot create clipboard window" << std::endl;
        return false;
    }

    // Set override_redirect to prevent WM interference
    XSetWindowAttributes clipAttrs;
    clipAttrs.override_redirect = True;
    XChangeWindowAttributes(display, g_clipboardWin, CWOverrideRedirect, &clipAttrs);

    XSelectInput(display, g_clipboardWin, SelectionClear | SelectionRequest);
    XMapWindow(display, g_clipboardWin);

    Atom clipboardAtom = XInternAtom(display, "CLIPBOARD", False);
    g_clipboardTimestamp = CurrentTime;
    XSetSelectionOwner(display, clipboardAtom, g_clipboardWin, g_clipboardTimestamp);

    if (XGetSelectionOwner(display, clipboardAtom) != g_clipboardWin) {
        std::cerr << "Error: Cannot set clipboard owner" << std::endl;
        XDestroyWindow(display, g_clipboardWin);
        return false;
    }

    g_clipboardActive = true;
    g_clipboardTimeout = 30000;

    XFlush(display);

    std::cout << "Image copied to clipboard" << std::endl;
    return true;
}
