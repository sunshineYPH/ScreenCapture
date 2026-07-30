#include "button_icons.h"
#include "png_io.h"

#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <libgen.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace button_icons {

namespace {

// Per-icon cache entry. The Pixmap is a 32-bit depth Pixmap
// containing pre-multiplied ARGB pixels (the layout XRender
// expects for PictStandardARGB32). The Picture is the XRender
// handle that the renderer uses to composite the icon onto the
// destination — looking it up once and storing it avoids
// re-creating the Picture on every menu repaint.
struct Icon {
    Pixmap  pixmap  = None;
    Picture picture = None;
    int width  = 0;
    int height = 0;
};

const std::unordered_map<std::string, std::string>& labelToFile() {
    static const std::unordered_map<std::string, std::string> m = {
        {"Confirm", "confirm"},
        {"Cancel",  "cancel"},
        {"Save",    "save"},
        {"Pin",     "pin"},
        {"Arrow",   "arrow"},
        {"Rect",    "rect"},
        {"Ellipse", "circle"},
        {"Text",    "text"},
        {"Color",   "color"},
        {"Size",    "font_size"},
        {"Undo",    "undo"},
        {"Redo",    "redo"},   // asset is "redo.png"
        {"Cut",     "cut"},
    };
    return m;
}

Display*    g_display    = nullptr;
int         g_screen     = 0;
int         g_targetSize = 24;
std::string g_assetsDir  = "./assets";
std::unordered_map<std::string, Icon> g_icons;

bool isDirectory(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

// Build a 32-bit ARGB Pixmap from straight-alpha RGBA bytes.
// XRender's PictStandardARGB32 format requires pre-multiplied
// alpha and byte order B,G,R,A in memory (which is what
// 0xAARRGGBB looks like as a little-endian uint32 — same layout
// on big-endian too because uint32 endianness is the
// same as the host's, just with the bytes shuffled).
//
// Pre-multiplication is critical: a pixel with alpha=128 and
// R=255 must be stored as R=128, not R=255. If we forgot to
// pre-multiply, semi-transparent edges would render incorrectly
// (e.g. an orange icon on a gray button would show as orange +
// half gray, instead of the correct "anti-aliased blend").
Pixmap buildARGBPixmap(int w, int h, const std::vector<uint8_t>& rgba) {
    Pixmap pm = XCreatePixmap(g_display, RootWindow(g_display, g_screen),
                              w, h, 32);
    if (pm == None) return None;

    // XImage data: 32-bit pixels, no per-row padding needed
    // (bitmap_pad=32 means each row is padded to a 32-bit
    // boundary, which is already the case at 4 bytes per pixel).
    char* data = static_cast<char*>(std::malloc(static_cast<size_t>(w) * h * 4));
    if (!data) { XFreePixmap(g_display, pm); return None; }

    for (int y = 0; y < h; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(data + y * w * 4);
        const uint8_t* src = rgba.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; x++) {
            uint8_t r = src[x * 4 + 0];
            uint8_t g = src[x * 4 + 1];
            uint8_t b = src[x * 4 + 2];
            uint8_t a = src[x * 4 + 3];
            // Pre-multiply: pr = r * a / 255. The +127 rounds
            // (without it, division always truncates).
            uint8_t pr = static_cast<uint8_t>((r * a + 127) / 255);
            uint8_t pg = static_cast<uint8_t>((g * a + 127) / 255);
            uint8_t pb = static_cast<uint8_t>((b * a + 127) / 255);
            row[x] = (static_cast<uint32_t>(a) << 24) |
                     (static_cast<uint32_t>(pr) << 16) |
                     (static_cast<uint32_t>(pg) << 8)  |
                     static_cast<uint32_t>(pb);
        }
    }

    XImage* img = XCreateImage(g_display, DefaultVisual(g_display, g_screen),
                               32, ZPixmap, 0, data, w, h, 32, 0);
    if (!img) { std::free(data); XFreePixmap(g_display, pm); return None; }

    GC gc = XCreateGC(g_display, pm, 0, nullptr);
    XPutImage(g_display, pm, gc, img, 0, 0, 0, 0, w, h);
    XFreeGC(g_display, gc);
    // XDestroyImage frees the malloc'd data buffer.
    XDestroyImage(img);
    return pm;
}

bool loadOne(const std::string& label, const std::string& basename) {
    std::string path = g_assetsDir + "/" + basename + ".png";
    DecodedPNG png = decodePNGFile(path);
    if (png.width == 0 || png.height == 0) {
        std::fprintf(stderr, "[icons] '%s' not loaded (path=%s)\n",
                     label.c_str(), path.c_str());
        return false;
    }

    // Box-average downscale. 200x200 -> 24x24 averages an ~8x8
    // block per destination pixel — smooth enough that one
    // source anti-aliased edge becomes a 1-pixel anti-aliased
    // edge in the result. Important for transparency: a partial-
    // alpha edge in the source averages with surrounding
    // transparent pixels to a smaller alpha, which is what we
    // want for the result to look like a smooth shape.
    const int dw = g_targetSize, dh = g_targetSize;
    std::vector<uint8_t> scaled(static_cast<size_t>(dw) * dh * 4, 0);
    for (int dy = 0; dy < dh; dy++) {
        int sy0 = dy * png.height / dh;
        int sy1 = std::max(sy0 + 1, (dy + 1) * png.height / dh);
        for (int dx = 0; dx < dw; dx++) {
            int sx0 = dx * png.width / dw;
            int sx1 = std::max(sx0 + 1, (dx + 1) * png.width / dw);
            uint32_t accA = 0, accR = 0, accG = 0, accB = 0;
            int count = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                for (int sx = sx0; sx < sx1; sx++) {
                    const uint8_t* p = &png.rgba[(static_cast<size_t>(sy) * png.width + sx) * 4];
                    accA += p[3]; accR += p[0]; accG += p[1]; accB += p[2];
                    count++;
                }
            }
            uint8_t* d = &scaled[(static_cast<size_t>(dy) * dw + dx) * 4];
            d[3] = static_cast<uint8_t>(accA / count);
            d[0] = static_cast<uint8_t>(accR / count);
            d[1] = static_cast<uint8_t>(accG / count);
            d[2] = static_cast<uint8_t>(accB / count);
        }
    }

    Pixmap pm = buildARGBPixmap(dw, dh, scaled);
    if (pm == None) return false;

    // Wrap the Pixmap in a Picture. XRender needs Pictures (not
    // Pixmaps) as source for PictOpOver. The format must match
    // the Pixmap's depth and layout: depth 32 + ARGB32.
    XRenderPictFormat* fmt = XRenderFindStandardFormat(g_display, PictStandardARGB32);
    Picture pic = None;
    if (fmt) {
        pic = XRenderCreatePicture(g_display, pm, fmt, 0, nullptr);
    }

    Icon& ic = g_icons[label];
    if (ic.picture != None) XRenderFreePicture(g_display, ic.picture);
    if (ic.pixmap  != None) XFreePixmap(g_display, ic.pixmap);
    ic.pixmap  = pm;
    ic.picture = pic;
    ic.width   = dw;
    ic.height  = dh;
    return true;
}

// Cached "destination Picture" for the screen-capture buffer.
// Re-creating a Picture on every XRenderComposite call is
// measurable overhead, so we keep one around. The cache is
// invalidated by cleanup() and by calls from a different
// Drawable.
Drawable          g_lastDest   = None;
Picture           g_destPic    = None;

// Cache of 1x1 solid-color Pixmaps used by drawIconHighlight.
// Lifted to namespace scope so cleanup() can free them at
// shutdown, before the X display goes away. If we left this as
// a function-static std::unordered_map, its destructor would
// run at program exit, by which time g_display is nullptr and
// XFreePixmap would crash with "munmap_chunk(): invalid pointer".
std::unordered_map<int, Pixmap> g_solidCache;

void ensureDestPicture(Display* display, Drawable dest) {
    if (g_lastDest == dest && g_destPic != None) return;
    if (g_destPic != None) {
        XRenderFreePicture(display, g_destPic);
        g_destPic = None;
    }
    XRenderPictFormat* fmt = XRenderFindVisualFormat(display,
        DefaultVisual(display, DefaultScreen(display)));
    if (!fmt) return;
    g_destPic = XRenderCreatePicture(display, dest, fmt, 0, nullptr);
    g_lastDest = dest;
}

} // namespace

void init(Display* display, int screen,
          const std::string& assetsDir, int targetSize) {
    g_display    = display;
    g_screen     = screen;
    g_targetSize = targetSize;

    // Asset directory resolution.
    //
    // We MUST NOT use paths relative to the current working
    // directory. The cwd of this process is whatever the user
    // launched from (a terminal, a desktop file, an IDE), and
    // assuming "we're running from build/" is fragile and
    // breaks the moment the binary is moved or installed.
    //
    // Instead, we always resolve paths from the binary's own
    // location (/proc/self/exe -> dirname -> exeDir). This
    // works for every launch scenario:
    //
    //   ./build/screen_capture  (development)
    //     -> exeDir = .../build
    //     -> assets = .../build/assets
    //
    //   /usr/bin/screen_capture  (system install)
    //     -> exeDir = /usr/bin
    //     -> assets = /usr/share/screen_capture/assets
    //
    // The caller-supplied assetsDir is intentionally ignored
    // here — keeping it as a parameter avoids changing the
    // public API, but it has no effect.
    char exeBuf[4096] = {0};
    std::string exeDir;
    {
        ssize_t n = ::readlink("/proc/self/exe", exeBuf,
                                sizeof(exeBuf) - 1);
        if (n > 0) {
            exeBuf[n] = '\0';
            char* d = ::dirname(exeBuf);
            if (d) exeDir = d;
        }
    }
    if (exeDir.empty()) exeDir = ".";

    // Each candidate is built as an absolute path joined to
    // exeDir. We check isDirectory for each and use the first
    // match. Order matters: most-specific (next to the
    // binary) first, system-wide paths last.
    std::vector<std::string> candidates;
    candidates.push_back(exeDir + "/assets");
    candidates.push_back(exeDir + "/../assets");
    candidates.push_back(exeDir + "/../share/screen_capture/assets");
    candidates.push_back(exeDir + "/share/screen_capture/assets");
    candidates.push_back("/usr/share/screen_capture/assets");
    candidates.push_back("/usr/local/share/screen_capture/assets");

    bool found = false;
    for (const std::string& p : candidates) {
        if (isDirectory(p)) {
            // Canonicalize (resolve "..", ".") so downstream
            // path joins in loadOne() are clean. We use
            // realpath() which returns the absolute path with
            // all symlinks resolved.
            char realBuf[PATH_MAX] = {0};
            if (::realpath(p.c_str(), realBuf) != nullptr) {
                g_assetsDir = realBuf;
            } else {
                g_assetsDir = p;
            }
            found = true;
            break;
        }
    }
    // Last-ditch fallback so loadOne() at least produces
    // visible "not loaded" warnings rather than silently
    // using an empty path. We point at the binary-adjacent
    // assets path even if it doesn't exist yet — the user
    // will see clear error output and can fix the layout.
    if (!found) {
        g_assetsDir = exeDir + "/assets";
        std::fprintf(stderr,
                     "[icons] WARNING: no assets directory found. "
                     "Tried these paths relative to binary at %s:\n",
                     exeDir.c_str());
        for (const std::string& p : candidates) {
            std::fprintf(stderr, "[icons]   %s\n", p.c_str());
        }
    }
    // Suppress the "unused parameter" warning for assetsDir.
    // It's part of the public API; see comment above.
    (void)assetsDir;

    cleanup();
    for (const auto& kv : labelToFile()) {
        loadOne(kv.first, kv.second);
    }
}

void cleanup() {
    if (!g_display) return;
    for (auto& kv : g_icons) {
        if (kv.second.picture != None) XRenderFreePicture(g_display, kv.second.picture);
        if (kv.second.pixmap  != None) XFreePixmap(g_display, kv.second.pixmap);
        kv.second.pixmap  = None;
        kv.second.picture = None;
    }
    g_icons.clear();
    if (g_destPic != None) {
        XRenderFreePicture(g_display, g_destPic);
        g_destPic = None;
    }
    g_lastDest = None;
    // Free the 1x1 solid-color Pixmaps used by drawIconHighlight.
    // Must happen here, before main() closes the X display, or
    // static destruction of g_solidCache at program exit will
    // try to XFreePixmap on a dead display and crash with
    // "munmap_chunk(): invalid pointer".
    for (auto& kv : g_solidCache) {
        if (kv.second != None) XFreePixmap(g_display, kv.second);
    }
    g_solidCache.clear();
}

bool get(const std::string& label, Pixmap* pixmap, int* w, int* h) {
    auto it = g_icons.find(label);
    if (it == g_icons.end() || it->second.pixmap == None) return false;
    if (pixmap) *pixmap = it->second.pixmap;
    if (w)      *w      = it->second.width;
    if (h)      *h      = it->second.height;
    return true;
}

void drawIcon(Display* display, Drawable dest, GC /*gc*/,
              Pixmap icon, int dstX, int dstY, int w, int h) {
    auto it = std::find_if(g_icons.begin(), g_icons.end(),
        [icon](const std::pair<std::string, Icon>& kv) {
            return kv.second.pixmap == icon;
        });
    if (it == g_icons.end() || it->second.picture == None) return;

    ensureDestPicture(display, dest);
    if (g_destPic == None) return;

    // PictOpOver: standard "source over destination" alpha
    // blend. With pre-multiplied source alpha, a=0 leaves the
    // destination unchanged and a=255 replaces it.
    XRenderComposite(display, PictOpOver,
                     it->second.picture, None, g_destPic,
                     0, 0, 0, 0,
                     dstX, dstY,
                     static_cast<unsigned>(w), static_cast<unsigned>(h));
}

void drawIconHighlight(Display* display, Drawable dest, Pixmap icon,
                       int dstX, int dstY, int w, int h,
                       int r, int g, int b, int alpha) {
    auto it = std::find_if(g_icons.begin(), g_icons.end(),
        [icon](const std::pair<std::string, Icon>& kv) {
            return kv.second.pixmap == icon;
        });
    if (it == g_icons.end() || it->second.picture == None) return;

    ensureDestPicture(display, dest);
    if (g_destPic == None) return;

    // We want to paint a solid (r, g, b) color over only the
    // icon's figure pixels (the parts of the source that have
    // alpha > 0). XRender supports this with the "mask Picture"
    // argument to XRenderComposite: src is a solid-color
    // Picture, mask is the icon's own Picture, and the mask's
    // alpha channel modulates the src's contribution.
    //
    // Pre-multiply the color by the caller's alpha so the
    // resulting overlay is at the requested strength (since
    // PictOpOver + the mask's own alpha give us multiplication
    // of two alphas, not a "constant" overlay).
    int a = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
    int pr = (r * a + 127) / 255;
    int pg = (g * a + 127) / 255;
    int pb = (b * a + 127) / 255;

    // Build (or look up) a 1x1 solid-color ARGB32 Pixmap. The
    // alpha stored in the pixel must be 255 — the PictOpOver
    // with the icon's own alpha as mask already provides the
    // per-pixel modulation. We use the namespace-scope cache
    // g_solidCache so cleanup() can free these at shutdown.
    //
    // The data buffer passed to XCreateImage is freed by
    // XDestroyImage via free(), so it MUST be heap-allocated
    // (std::malloc). A stack array here would cause
    // "free(): invalid pointer" the first time we draw a
    // highlight.
    int key = (pr << 16) | (pg << 8) | pb;
    Pixmap solid = None;
    auto cacheIt = g_solidCache.find(key);
    if (cacheIt != g_solidCache.end()) {
        solid = cacheIt->second;
    } else {
        solid = XCreatePixmap(g_display, RootWindow(g_display, g_screen),
                              1, 1, 32);
        if (solid == None) return;
        uint32_t pixel = (255u << 24) | (uint32_t(pr) << 16) |
                         (uint32_t(pg) << 8)  | uint32_t(pb);
        // Heap-allocate so XDestroyImage's free() is valid.
        char* data = static_cast<char*>(std::malloc(4));
        if (!data) { XFreePixmap(g_display, solid); return; }
        std::memcpy(data, &pixel, 4);
        XImage* img = XCreateImage(g_display, DefaultVisual(g_display, g_screen),
                                   32, ZPixmap, 0, data, 1, 1, 32, 0);
        if (!img) { std::free(data); XFreePixmap(g_display, solid); return; }
        GC gc = XCreateGC(g_display, solid, 0, nullptr);
        XPutImage(g_display, solid, gc, img, 0, 0, 0, 0, 1, 1);
        XFreeGC(g_display, gc);
        // XDestroyImage frees the malloc'd data buffer.
        XDestroyImage(img);
        g_solidCache[key] = solid;
    }

    XRenderPictFormat* fmt = XRenderFindStandardFormat(g_display, PictStandardARGB32);
    if (!fmt) return;
    Picture src = XRenderCreatePicture(g_display, solid, fmt, 0, nullptr);
    if (src == None) return;

    // XRenderComposite's mask argument is the icon's own
    // picture — its alpha channel is applied to the source
    // pixel. With PictOpOver, the result is: the destination
    // gets a fraction of (r, g, b) at each pixel, where the
    // fraction equals the icon's own alpha / 255. So transparent
    // background pixels get no highlight, figure pixels get the
    // full pre-multiplied color.
    XRenderComposite(display, PictOpOver,
                     src, it->second.picture, g_destPic,
                     0, 0, 0, 0,
                     dstX, dstY,
                     static_cast<unsigned>(w), static_cast<unsigned>(h));

    // Source Picture is tiny and re-creating it costs a round
    // trip. We keep a per-(solid, fmt) cache so repeated
    // hovers don't churn XRender resources. Note: this
    // Picture is freed in cleanup() (we drop g_srcPics).
    // The key encodes both the solid Pixmap ID and the format
    // so we don't accidentally mismatch.
    // (For now we create and free per call; the cost is small
    // and a Picture leak here would compound. Better safe.)
    XRenderFreePicture(display, src);
}

std::vector<std::string> availableLabels() {
    std::vector<std::string> out;
    out.reserve(g_icons.size());
    for (const auto& kv : g_icons) {
        if (kv.second.pixmap != None) out.push_back(kv.first);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace button_icons
