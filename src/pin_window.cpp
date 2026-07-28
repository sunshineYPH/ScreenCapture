#include "pin_window.h"

#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace {

// Allocates a pixel via the default colormap and sets it as the GC foreground.
void setGCColorLocal(Display* display, GC gc, int r, int g, int b) {
    Colormap cmap = DefaultColormap(display, DefaultScreen(display));
    XColor xcolor;
    xcolor.red = r * 257;
    xcolor.green = g * 257;
    xcolor.blue = b * 257;
    xcolor.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(display, cmap, &xcolor);
    XSetForeground(display, gc, xcolor.pixel);
}

// Encodes the given XImage as a binary PPM (P6) file. PPM is trivial to encode
// and decode so it works without external image libraries.
bool writePPM(const char* path, XImage* img, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    fprintf(f, "P6\n%d %d\n255\n", w, h);

    std::vector<unsigned char> rowBuf(w * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned long pixel = XGetPixel(img, x, y);
            rowBuf[x * 3 + 0] = (pixel >> 16) & 0xFF; // R
            rowBuf[x * 3 + 1] = (pixel >> 8) & 0xFF;  // G
            rowBuf[x * 3 + 2] = pixel & 0xFF;         // B
        }
        fwrite(rowBuf.data(), 1, rowBuf.size(), f);
    }

    fclose(f);
    return true;
}

} // namespace

bool pinToScreen(Display* display, Renderer* renderer,
                 int selX, int selY, int selW, int selH) {
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    Pixmap pixmap = XCreatePixmap(display, root, selW, selH, 24);
    if (!pixmap) {
        std::cerr << "Error: Cannot create pixmap for pin" << std::endl;
        return false;
    }

    renderer->renderToPixmap(pixmap, selX, selY, selW, selH);

    XImage* img = XGetImage(display, pixmap, 0, 0, selW, selH, AllPlanes, ZPixmap);
    XFreePixmap(display, pixmap);
    if (!img) {
        std::cerr << "Error: Cannot get image for pin" << std::endl;
        return false;
    }

    char ppmPath[256];
    static int pinCounter = 0;
    snprintf(ppmPath, sizeof(ppmPath), "/tmp/screen_capture_pin_%d_%d.ppm",
             getpid(), pinCounter++);

    if (!writePPM(ppmPath, img, selW, selH)) {
        std::cerr << "Error: Cannot write PPM file: " << ppmPath << std::endl;
        XDestroyImage(img);
        return false;
    }
    XDestroyImage(img);

    char exePath[4096];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0) {
        std::cerr << "Error: Cannot get executable path" << std::endl;
        unlink(ppmPath);
        return false;
    }
    exePath[len] = '\0';

    pid_t pid = fork();
    if (pid == 0) {
        // Child: replace ourselves with the pin-window variant of this binary.
        execl(exePath, "screen_capture", "--pin", ppmPath, (char*)nullptr);
        _exit(1);
    }
    if (pid < 0) {
        std::cerr << "Error: fork failed" << std::endl;
        unlink(ppmPath);
        return false;
    }

    std::cout << "Screenshot pinned (child PID: " << pid << ")" << std::endl;
    return true;
}

int runPinWindow(const char* ppmPath) {
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "Error: Cannot open X11 display" << std::endl;
        unlink(ppmPath);
        return 1;
    }

    FILE* f = fopen(ppmPath, "rb");
    if (!f) {
        std::cerr << "Error: Cannot open PPM file: " << ppmPath << std::endl;
        XCloseDisplay(display);
        return 1;
    }

    char magic[3] = {0};
    int w = 0, h = 0, maxVal = 0;
    if (fscanf(f, "%2s %d %d %d ", magic, &w, &h, &maxVal) != 4 ||
        strcmp(magic, "P6") != 0 || w <= 0 || h <= 0) {
        std::cerr << "Error: Invalid PPM file" << std::endl;
        fclose(f);
        XCloseDisplay(display);
        unlink(ppmPath);
        return 1;
    }

    int rgbRowBytes = w * 3;
    std::vector<unsigned char> rgb((size_t)h * rgbRowBytes);
    if (fread(rgb.data(), 1, rgb.size(), f) != rgb.size()) {
        std::cerr << "Error: Cannot read PPM data" << std::endl;
        fclose(f);
        XCloseDisplay(display);
        unlink(ppmPath);
        return 1;
    }
    fclose(f);

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    Visual* visual = DefaultVisual(display, screen);
    int depth = DefaultDepth(display, screen);

    XImage* img = XCreateImage(display, visual, depth, ZPixmap, 0, nullptr, w, h, 32, 0);
    if (!img) {
        std::cerr << "Error: Cannot create XImage" << std::endl;
        XCloseDisplay(display);
        unlink(ppmPath);
        return 1;
    }

    // XCreateImage with a NULL data pointer is documented to allocate, but in
    // practice some X11 implementations do not (or allocate zero bytes). Always
    // allocate ourselves and zero-initialize so the XPutImage below is well-defined.
    img->data = (char*)malloc((size_t)img->bytes_per_line * h);
    if (!img->data) {
        std::cerr << "Error: Cannot allocate XImage data" << std::endl;
        XDestroyImage(img);
        XCloseDisplay(display);
        unlink(ppmPath);
        return 1;
    }
    memset(img->data, 0, (size_t)img->bytes_per_line * h);

    // Use XPutPixel so the byte order / depth conversion is handled by Xlib
    // rather than us writing raw bytes in a possibly-wrong format.
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned long r = rgb[y * rgbRowBytes + x * 3 + 0];
            unsigned long g = rgb[y * rgbRowBytes + x * 3 + 1];
            unsigned long b = rgb[y * rgbRowBytes + x * 3 + 2];
            XPutPixel(img, x, y, (r << 16) | (g << 8) | b);
        }
    }

    // Border around the pinned image. The window is (w + 2*border) x (h + 2*border);
    // the image sits at offset (border, border); the border pixels are filled
    // with a visible accent color so the screenshot looks framed.
    const int border = 2;
    const int winW = w + 2 * border;
    const int winH = h + 2 * border;
    Pixmap pinPixmap = XCreatePixmap(display, root, winW, winH, depth);

    // Fill border with accent color first.
    GC borderGC = XCreateGC(display, pinPixmap, 0, nullptr);
    setGCColorLocal(display, borderGC, 0, 122, 255);
    XFillRectangle(display, pinPixmap, borderGC, 0, 0, winW, winH);
    XFreeGC(display, borderGC);

    // Blit the image into the center, leaving the border visible.
    GC imgGC = XCreateGC(display, pinPixmap, 0, nullptr);
    XPutImage(display, pinPixmap, imgGC, img, 0, 0, border, border, w, h);
    XFreeGC(display, imgGC);

    // Close button: 16x16 black square in the top-right corner of the image
    // area with a white X. Coordinates are relative to the window (border
    // included), so the X is at (border + (w - btnSize - btnMargin), border + btnMargin).
    const int btnSize = 16;
    const int btnMargin = 2;
    int btnX = border + w - btnSize - btnMargin;
    int btnY = border + btnMargin;
    if (w >= btnSize + btnMargin * 2 && h >= btnSize + btnMargin * 2) {
        GC drawGC = XCreateGC(display, pinPixmap, 0, nullptr);
        setGCColorLocal(display, drawGC, 0, 0, 0);
        XFillRectangle(display, pinPixmap, drawGC, btnX, btnY, btnSize, btnSize);
        setGCColorLocal(display, drawGC, 255, 255, 255);
        XSetLineAttributes(display, drawGC, 2, LineSolid, CapRound, JoinRound);
        XDrawLine(display, pinPixmap, drawGC, btnX + 3, btnY + 3,
                  btnX + btnSize - 4, btnY + btnSize - 4);
        XDrawLine(display, pinPixmap, drawGC, btnX + btnSize - 4, btnY + 3,
                  btnX + 3, btnY + btnSize - 4);
        XFreeGC(display, drawGC);
    } else {
        btnX = btnY = -1; // disable close-button hit testing for tiny images
    }

    int initialX = 100;
    int initialY = 100;
    if (initialX + winW > DisplayWidth(display, screen))  initialX = 50;
    if (initialY + winH > DisplayHeight(display, screen)) initialY = 50;

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.event_mask = ButtonPressMask | ButtonReleaseMask |
                       Button1MotionMask | PointerMotionMask |
                       KeyPressMask | ExposureMask;
    attrs.background_pixmap = pinPixmap;
    attrs.colormap = DefaultColormap(display, screen);

    Window pinWin = XCreateWindow(display, root,
        initialX, initialY, winW, winH, 0,
        depth, InputOutput, visual,
        CWOverrideRedirect | CWEventMask | CWBackPixmap | CWColormap,
        &attrs);

    if (!pinWin) {
        std::cerr << "Error: Cannot create pin window" << std::endl;
        XDestroyImage(img);
        XFreePixmap(display, pinPixmap);
        XCloseDisplay(display);
        unlink(ppmPath);
        return 1;
    }

    XSetWindowBackgroundPixmap(display, pinWin, pinPixmap);
    XMapRaised(display, pinWin);
    XFlush(display);

    bool dragging = false;
    int dragOffsetX = 0, dragOffsetY = 0;
    int winStartX = 0, winStartY = 0;
    XEvent ev;

    auto closeAndExit = [&]() {
        XDestroyImage(img);
        XFreePixmap(display, pinPixmap);
        XDestroyWindow(display, pinWin);
        XCloseDisplay(display);
        unlink(ppmPath);
    };

    while (true) {
        XNextEvent(display, &ev);
        switch (ev.type) {
            case ButtonPress:
                if (ev.xbutton.button == Button1) {
                    int mx = ev.xbutton.x;
                    int my = ev.xbutton.y;
                    if (btnX >= 0 && mx >= btnX && mx < btnX + btnSize &&
                        my >= btnY && my < btnY + btnSize) {
                        closeAndExit();
                        return 0;
                    }
                    dragging = true;
                    dragOffsetX = mx;
                    dragOffsetY = my;
                    Window child;
                    XTranslateCoordinates(display, pinWin, root,
                        0, 0, &winStartX, &winStartY, &child);
                }
                break;
            case MotionNotify:
                if (dragging) {
                    int newX = ev.xmotion.x_root - dragOffsetX;
                    int newY = ev.xmotion.y_root - dragOffsetY;
                    XMoveWindow(display, pinWin, newX, newY);
                }
                break;
            case ButtonRelease:
                if (ev.xbutton.button == Button1) {
                    dragging = false;
                }
                break;
            case KeyPress: {
                KeySym key = XLookupKeysym(&ev.xkey, 0);
                if (key == XK_Escape) {
                    closeAndExit();
                    return 0;
                }
                break;
            }
        }
    }
}
