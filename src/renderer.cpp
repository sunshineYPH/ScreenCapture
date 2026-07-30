#include "renderer.h"
#include "button_icons.h"
#include <X11/Xutil.h>
#include <cmath>
#include <cstring>

const DrawColor AVAILABLE_COLORS[] = {
    {255, 0, 0, "Red"},
    {255, 255, 0, "Yellow"},
    {0, 0, 255, "Blue"},
    {0, 255, 0, "Green"},
    {128, 128, 128, "Gray"},
    {255, 255, 255, "White"},
    {0, 0, 0, "Black"}
};
const int NUM_COLORS = 7;

static void setGCColor(Display* display, GC gc, int r, int g, int b) {
    Colormap cmap = DefaultColormap(display, DefaultScreen(display));
    XColor xcolor;
    xcolor.red = r * 257;
    xcolor.green = g * 257;
    xcolor.blue = b * 257;
    xcolor.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(display, cmap, &xcolor);
    XSetForeground(display, gc, xcolor.pixel);
}

Renderer::Renderer(Display* display, Window window, ScreenCapture* capture)
    // Member init order MUST match the declaration order in renderer.h to
    // silence -Wreorder. Members without an explicit init here are
    // value-initialized (vectors/strings/structs) or default-initialized
    // (the small ints) and don't need a clause.
    : m_display(display)
    , m_window(window)
    , m_gc(nullptr)
    , m_capture(capture)
    , m_bufferGC(nullptr)
    , m_winGC(nullptr)
    , m_font(nullptr)
    , m_fontLoaded(false)
    , m_submenuFont(nullptr)
    , m_fontset(nullptr)
    , m_fontsetLoaded(false)
    , m_selX(0), m_selY(0), m_selWidth(0), m_selHeight(0)
    , m_hasSelection(false)
    , m_showRGBPanel(false)
    , m_toolMode(ToolMode::NONE)
    , m_fontSize(16)
    , m_drawingArrow(false)
    , m_textInput(false)
    , m_textInputX(0), m_textInputY(0)
    , m_drawState(DrawState::IDLE)
    , m_tempX1(0), m_tempY1(0), m_tempX2(0), m_tempY2(0)
    , m_historyIndex(-1)
    , m_buffer(None)
    , m_dimmedImage(nullptr)
{
    m_gc = XCreateGC(display, window, 0, nullptr);
    m_currentColor = AVAILABLE_COLORS[0];

    m_colorSubMenu.visible = false;
    m_colorSubMenu.hoveredIndex = -1;
    m_colorSubMenu.itemHeight = 24;
    m_colorSubMenu.itemWidth = 80;

    for (int i = 0; i < NUM_COLORS; i++) {
        m_colorSubMenu.colors.push_back({AVAILABLE_COLORS[i].name, AVAILABLE_COLORS[i]});
    }

    m_sizeSubMenu.visible = false;
    m_sizeSubMenu.hoveredIndex = -1;
    m_sizeSubMenu.itemHeight = 30;
    // Wider than the text needs because the rows now also
    // show a 10x10 radio button on the left, with 8px of
    // padding on each side of the dot, and 8px of padding
    // after the dot before the label. 110 is enough for
    // "medium" + 24px dot + padding.
    m_sizeSubMenu.itemWidth = 110;
    // Three preset sizes — small/medium/big. Display label
    // intentionally omits the "pt" suffix; the user picks the
    // size by name only, and the actual font size is set on
    // the renderer.
    m_sizeSubMenu.sizes.push_back({"small",  16});
    m_sizeSubMenu.sizes.push_back({"medium", 24});
    m_sizeSubMenu.sizes.push_back({"big",    32});

    // Load the default size's font. setFontSize also handles
    // 16 -> 9x15, 24 -> 12x24, 32 -> 16x32 mapping.
    setFontSize(m_fontSize);

    saveHistory();
}

Renderer::~Renderer() {
    if (m_gc) XFreeGC(m_display, m_gc);
    if (m_bufferGC) XFreeGC(m_display, m_bufferGC);
    if (m_winGC) XFreeGC(m_display, m_winGC);
    if (m_buffer != None) XFreePixmap(m_display, m_buffer);
    if (m_dimmedImage) XDestroyImage(m_dimmedImage);
    // Note: m_font points into m_fontsBySize for the current
    // size, so XFreeFont on it would double-free. We free
    // the cache first, then null the active pointer to
    // prevent the (now stale) free below.
    for (auto& kv : m_fontsBySize) {
        if (kv.second) XFreeFont(m_display, kv.second);
    }
    m_fontsBySize.clear();
    m_font = nullptr;
    if (m_submenuFont) XFreeFont(m_display, m_submenuFont);
    if (m_fontset) XFreeFontSet(m_display, m_fontset);
}

void Renderer::ensureFallbackFont() {
    // The active bitmap font (m_font) is now managed by
    // getFontForSize / setFontSize, which cache every
    // requested size. This function is kept for any caller
    // that still wants a guaranteed non-null m_font; it
    // simply triggers a load at the current size.
    if (m_fontLoaded && m_font) return;
    m_font = getFontForSize(m_fontSize);
    m_fontLoaded = (m_font != nullptr);
}

XFontStruct* Renderer::getFontForSize(int size) {
    // Fast path — already cached.
    auto it = m_fontsBySize.find(size);
    if (it != m_fontsBySize.end()) return it->second;

    // Map requested pixel size to an X core bitmap font
    // that actually exists on this system. The pcf.gz files
    // under /usr/share/fonts/X11/misc/ on a typical Linux
    // desktop are: 4x6, 5x7, 5x8, 6x9..6x13, 7x13..7x14,
    // 8x13..8x16, 9x15, 9x18, 10x20, 12x24. Anything else
    // (16x32, 18x18) is aliased to 6x13 by the X server and
    // is therefore useless for size selection.
    const char* fontName = "9x15";  // 15px — small default
    if (size >= 20 && size < 30) fontName = "10x20";  // 20px — medium
    else if (size >= 30)         fontName = "12x24";  // 24px — big
    XFontStruct* f = XLoadQueryFont(m_display, fontName);
    if (!f) {
        // Minimal systems might be missing 10x20 or 12x24.
        // Step down through the available sizes until one
        // loads.
        const char* fallbacks[] = {"10x20", "9x15", "fixed"};
        for (const char* fb : fallbacks) {
            f = XLoadQueryFont(m_display, fb);
            if (f) break;
        }
    }
    if (f) m_fontsBySize[size] = f;
    return f;
}

XFontStruct* Renderer::getSubmenuFont() {
    if (m_submenuFont) return m_submenuFont;
    // Always 9x15 — small, fixed, and unaffected by the
    // user's size choice. Submenu labels are metadata, not
    // content the user is composing.
    m_submenuFont = XLoadQueryFont(m_display, "9x15");
    if (!m_submenuFont) m_submenuFont = XLoadQueryFont(m_display, "fixed");
    return m_submenuFont;
}

void Renderer::initDoubleBuffer() {
    if (m_buffer != None) XFreePixmap(m_display, m_buffer);
    int screen = DefaultScreen(m_display);
    m_buffer = XCreatePixmap(m_display, m_window,
                             m_capture->getWidth(), m_capture->getHeight(),
                             DefaultDepth(m_display, screen));

    // Build the dimmed (semi-transparent gray) image once; reused on every
    // render so the per-frame cost is just XPutImage + the un-dimmed patch.
    createDimmedImage();
}

void Renderer::createDimmedImage() {
    if (m_dimmedImage) {
        XDestroyImage(m_dimmedImage);
        m_dimmedImage = nullptr;
    }

    XImage* src = m_capture->getImage();
    if (!src) return;

    int w = m_capture->getWidth();
    int h = m_capture->getHeight();
    int screen = DefaultScreen(m_display);
    Visual* visual = DefaultVisual(m_display, screen);
    int depth = DefaultDepth(m_display, screen);

    m_dimmedImage = XCreateImage(m_display, visual, depth, ZPixmap, 0,
                                 nullptr, w, h, 32, 0);
    if (!m_dimmedImage) return;

    // XCreateImage may not always allocate data when NULL is passed depending
    // on the X11 implementation. Allocate explicitly to be safe.
    m_dimmedImage->data = (char*)malloc(m_dimmedImage->bytes_per_line * h);
    if (!m_dimmedImage->data) {
        XDestroyImage(m_dimmedImage);
        m_dimmedImage = nullptr;
        return;
    }
    memset(m_dimmedImage->data, 0, m_dimmedImage->bytes_per_line * h);

    // 50% blend with RGB(40, 40, 40) — gives the dimmed look that
    // XRender-style alpha would produce without needing the extension.
    const int grayR = 40, grayG = 40, grayB = 40;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned long p = XGetPixel(src, x, y);
            int r = (((p >> 16) & 0xFF) + grayR) / 2;
            int g = (((p >> 8) & 0xFF) + grayG) / 2;
            int b = ((p & 0xFF) + grayB) / 2;
            XPutPixel(m_dimmedImage, x, y, (r << 16) | (g << 8) | b);
        }
    }
}

void Renderer::updateColorSubMenu(int baseX, int baseY) {
    m_colorSubMenu.x = baseX;
    m_colorSubMenu.y = baseY;
}

void Renderer::updateSizeSubMenu(int baseX, int baseY) {
    m_sizeSubMenu.x = baseX;
    m_sizeSubMenu.y = baseY;
}

void Renderer::render() {
    if (m_bufferGC == nullptr) {
        m_bufferGC = XCreateGC(m_display, m_buffer, 0, nullptr);
    }

    // Base layer: the cached dimmed (semi-transparent) image fills the screen.
    // drawGrayOverlayToBuffer() then restores the un-blended original only
    // inside the selection rectangle. This produces a true alpha-blended
    // dimmed look without needing XRender / an ARGB visual.
    if (m_dimmedImage) {
        XPutImage(m_display, m_buffer, m_bufferGC, m_dimmedImage,
                  0, 0, 0, 0, m_capture->getWidth(), m_capture->getHeight());
    } else if (m_capture->getImage()) {
        XPutImage(m_display, m_buffer, m_bufferGC, m_capture->getImage(),
                  0, 0, 0, 0, m_capture->getWidth(), m_capture->getHeight());
    }

    drawGrayOverlayToBuffer(m_bufferGC);

    if (m_hasSelection) {
        drawSelectionToBuffer(m_bufferGC);
    }

    if (m_showRGBPanel && m_hasSelection) {
        drawRGBPanelToBuffer(m_bufferGC);
    }

    if (!m_menuButtons.empty()) {
        drawMenuBarToBuffer(m_bufferGC);
    }

    if (m_colorSubMenu.visible) {
        drawColorSubMenuToBuffer(m_bufferGC);
    }
    if (m_sizeSubMenu.visible) {
        drawSizeSubMenuToBuffer(m_bufferGC);
    }

    drawArrowsToBuffer(m_bufferGC, m_arrows);
    drawRectanglesToBuffer(m_bufferGC, m_rectangles);
    drawEllipsesToBuffer(m_bufferGC, m_ellipses);
    drawTextsToBuffer(m_bufferGC, m_texts);
    drawTempShapeToBuffer(m_bufferGC);

    if (m_textInput) {
        drawTextInputBoxToBuffer();
    }

    if (m_winGC == nullptr) {
        m_winGC = XCreateGC(m_display, m_window, 0, nullptr);
    }
    XCopyArea(m_display, m_buffer, m_window, m_winGC,
              0, 0, m_capture->getWidth(), m_capture->getHeight(), 0, 0);
}

void Renderer::renderToPixmap(Pixmap pixmap, int selX, int selY, int selW, int selH) {
    GC gc = XCreateGC(m_display, pixmap, 0, nullptr);

    if (m_capture->getImage()) {
        XPutImage(m_display, pixmap, gc, m_capture->getImage(),
                  selX, selY, 0, 0, selW, selH);
    }

    for (const auto& arrow : m_arrows) {
        int ax1 = arrow.x1 - selX;
        int ay1 = arrow.y1 - selY;
        int ax2 = arrow.x2 - selX;
        int ay2 = arrow.y2 - selY;
        drawArrowLine(pixmap, gc, ax1, ay1, ax2, ay2, arrow.color);
    }

    for (const auto& rect : m_rectangles) {
        drawRect(pixmap, gc, rect.x - selX, rect.y - selY, rect.w, rect.h, rect.color);
    }

    for (const auto& ellipse : m_ellipses) {
        drawEllipse(pixmap, gc, ellipse.cx - selX, ellipse.cy - selY, ellipse.rx, ellipse.ry, ellipse.color);
    }

    for (const auto& text : m_texts) {
        int tx = text.x - selX;
        int ty = text.y - selY;
        drawTextString(pixmap, gc, tx, ty, text.text, text.color, text.fontSize);
    }

    XFreeGC(m_display, gc);
}

void Renderer::drawGrayOverlayToBuffer(GC gc) {
    // The base layer is already a dimmed (semi-transparent) version of the
    // full screen capture. Here we just restore the un-dimmed original image
    // inside the selection rectangle so the user sees the actual pixels in
    // that area. Nothing is drawn outside the selection — the dimmed base
    // already provides the semi-transparent mask there.
    (void)gc;
    if (!m_hasSelection || !m_capture->getImage()) return;

    int screenWidth = m_capture->getWidth();
    int screenHeight = m_capture->getHeight();

    int x = std::max(0, m_selX);
    int y = std::max(0, m_selY);
    int w = std::min(m_selX + m_selWidth, screenWidth) - x;
    int h = std::min(m_selY + m_selHeight, screenHeight) - y;
    if (w <= 0 || h <= 0) return;

    XPutImage(m_display, m_buffer, m_gc, m_capture->getImage(),
              x, y, x, y, (unsigned int)w, (unsigned int)h);
}

void Renderer::drawSelectionToBuffer(GC gc) {
    setGCColor(m_display, gc, 0, 122, 255);
    XSetLineAttributes(m_display, gc, 2, LineSolid, CapButt, JoinMiter);
    XDrawRectangle(m_display, m_buffer, gc, m_selX, m_selY, m_selWidth, m_selHeight);

    const int anchorSize = 6;
    setGCColor(m_display, gc, 255, 255, 255);

    int cx = m_selX + m_selWidth / 2;
    int cy = m_selY + m_selHeight / 2;

    XFillRectangle(m_display, m_buffer, gc, m_selX - anchorSize/2, m_selY - anchorSize/2, anchorSize, anchorSize);
    XFillRectangle(m_display, m_buffer, gc, m_selX + m_selWidth - anchorSize/2, m_selY - anchorSize/2, anchorSize, anchorSize);
    XFillRectangle(m_display, m_buffer, gc, m_selX - anchorSize/2, m_selY + m_selHeight - anchorSize/2, anchorSize, anchorSize);
    XFillRectangle(m_display, m_buffer, gc, m_selX + m_selWidth - anchorSize/2, m_selY + m_selHeight - anchorSize/2, anchorSize, anchorSize);

    XFillRectangle(m_display, m_buffer, gc, cx - anchorSize/2, m_selY - anchorSize/2, anchorSize, anchorSize);
    XFillRectangle(m_display, m_buffer, gc, cx - anchorSize/2, m_selY + m_selHeight - anchorSize/2, anchorSize, anchorSize);
    XFillRectangle(m_display, m_buffer, gc, m_selX - anchorSize/2, cy - anchorSize/2, anchorSize, anchorSize);
    XFillRectangle(m_display, m_buffer, gc, m_selX + m_selWidth - anchorSize/2, cy - anchorSize/2, anchorSize, anchorSize);
}

void Renderer::drawRGBPanelToBuffer(GC gc) {
    const int panelWidth = 100;
    const int panelHeight = 20;
    const int margin = 6;

    int panelX = m_selX + m_selWidth - panelWidth - margin;
    int panelY = m_selY + m_selHeight - panelHeight - margin;

    if (panelX < m_selX) panelX = m_selX;
    if (panelY < m_selY) panelY = m_selY;

    setGCColor(m_display, gc, 20, 20, 20);
    XFillRectangle(m_display, m_buffer, gc, panelX, panelY, panelWidth, panelHeight);

    setGCColor(m_display, gc, 100, 100, 100);
    XDrawRectangle(m_display, m_buffer, gc, panelX, panelY, panelWidth, panelHeight);

    char rgbText[64];
    snprintf(rgbText, sizeof(rgbText), "RGB: %d,%d,%d",
             m_currentPixel.r, m_currentPixel.g, m_currentPixel.b);

    setGCColor(m_display, gc, 255, 255, 255);
    if (!m_fontLoaded) {
        m_font = XLoadQueryFont(m_display, "9x15");
        m_fontLoaded = true;
    }
    if (m_font) {
        XSetFont(m_display, gc, m_font->fid);
        XDrawString(m_display, m_buffer, gc, panelX + 6, panelY + 14,
                    rgbText, (int)strlen(rgbText));
    }
}

void Renderer::drawMenuBarToBuffer(GC gc) {
    if (m_menuButtons.empty()) return;

    int totalWidth = 0;
    const int btnSpacing = 4;
    const int barPadH = 10;
    // Thin top/bottom padding (was 6) — the bar hugs the
    // buttons so it reads as a flat toolbar strip instead of
    // a chunky block with internal padding.
    const int barPadV = 0;

    for (const auto& btn : m_menuButtons) {
        totalWidth += btn.width + btnSpacing;
    }

    int barX = m_menuButtons[0].x - barPadH;
    int barY = m_menuButtons[0].y - barPadV;
    int barWidth = totalWidth + barPadH * 2;
    int barHeight = m_menuButtons[0].height + barPadV * 2;

    // Bar background: pure white. The screenshot selection
    // window overlays whatever is on the screen — a white bar
    // reads as a clean "tool palette" floating above the
    // capture.
    setGCColor(m_display, gc, 255, 255, 255);
    XFillRectangle(m_display, m_buffer, gc, barX, barY, barWidth, barHeight);

    // (Bar border removed — flat modern look.)

    // Each button has its own hover/press state. The button face
    // is filled with the state color, then the PNG icon is
    // composited on top via XRender. Hovering also brightens the
    // icon itself with a white alpha overlay so the user gets a
    // clear "this is the active button" cue.
    for (const auto& btn : m_menuButtons) {
        // Pick the button's fill color based on hover/press
        // state. The bar background is white (255,255,255), and
        // the unhovered button face is the same white — that
        // way the buttons are visually flat in their resting
        // state, indistinguishable from the bar background
        // until the user mouses over them. Hover/press then
        // darkens the button (light gray) to provide the
        // affordance. Going dark on hover is the inverse of the
        // dark-bar scheme but works correctly on a white
        // background.
        if (btn.pressed) {
            setGCColor(m_display, gc, 200, 200, 200);
        } else if (btn.hovered) {
            setGCColor(m_display, gc, 230, 230, 230);
        } else {
            setGCColor(m_display, gc, 255, 255, 255);
        }
        XFillRectangle(m_display, m_buffer, gc, btn.x, btn.y, btn.width, btn.height);

        // Draw the PNG icon for this button using XRender's
        // alpha-blended composite. The button's gray face is
        // already filled above, so a transparent icon background
        // blends with the gray and only the figure's pixels
        // contribute new color.
        Pixmap icon;
        int iw = 0, ih = 0;
        if (button_icons::get(btn.label, &icon, &iw, &ih)) {
            int ix = btn.x + (btn.width - iw) / 2;
            int iy = btn.y + (btn.height - ih) / 2;
            button_icons::drawIcon(m_display, m_buffer, gc, icon, ix, iy, iw, ih);
            // Hover/press highlight: paint a white overlay over
            // the icon at low alpha. The overlay only affects
            // the icon's figure pixels (where the source had
            // alpha > 0), so the surrounding button face stays
            // its normal color. This gives a clear, obvious
            // "active" state without changing the icon's shape
            // or underlying color identity.
            if (btn.hovered || btn.pressed) {
                button_icons::drawIconHighlight(m_display, m_buffer,
                                                 icon, ix, iy, iw, ih,
                                                 255, 255, 255,
                                                 btn.pressed ? 90 : 60);
            }
        } else {
            ensureFallbackFont();
            if (m_font) {
                setGCColor(m_display, gc, 255, 255, 255);
                XSetFont(m_display, gc, m_font->fid);
                int textWidth = XTextWidth(m_font, btn.label.c_str(), (int)btn.label.length());
                int textX = btn.x + (btn.width - textWidth) / 2;
                int textY = btn.y + (btn.height + m_font->ascent - m_font->descent) / 2;
                XDrawString(m_display, m_buffer, gc, textX, textY,
                            btn.label.c_str(), (int)btn.label.length());
            }
        }
    }
}

void Renderer::drawColorSubMenuToBuffer(GC gc) {
    if (!m_colorSubMenu.visible) return;

    int x = m_colorSubMenu.x;
    int y = m_colorSubMenu.y;
    int itemH = m_colorSubMenu.itemHeight;
    int itemW = m_colorSubMenu.itemWidth;
    int n = (int)m_colorSubMenu.colors.size();

    setGCColor(m_display, gc, 50, 50, 50);
    XFillRectangle(m_display, m_buffer, gc, x, y, itemW, itemH * n);

    setGCColor(m_display, gc, 100, 100, 100);
    XDrawRectangle(m_display, m_buffer, gc, x, y, itemW, itemH * n);

    if (!m_fontLoaded) {
        m_font = XLoadQueryFont(m_display, "9x15");
        m_fontLoaded = true;
    }
    XFontStruct* sm = getSubmenuFont();
    if (sm) {
        for (int i = 0; i < n; i++) {
            bool hovered = (i == m_colorSubMenu.hoveredIndex);
            int itemY = y + i * itemH;

            if (hovered) {
                setGCColor(m_display, gc, 80, 80, 80);
                XFillRectangle(m_display, m_buffer, gc, x + 1, itemY + 1, itemW - 2, itemH - 2);
            }

            setGCColor(m_display, gc, m_colorSubMenu.colors[i].second.r,
                       m_colorSubMenu.colors[i].second.g,
                       m_colorSubMenu.colors[i].second.b);
            XFillRectangle(m_display, m_buffer, gc, x + 4, itemY + 4, 16, itemH - 8);

            setGCColor(m_display, gc, 255, 255, 255);
            XSetFont(m_display, gc, sm->fid);
            XDrawString(m_display, m_buffer, gc, x + 24, itemY + itemH / 2 + sm->ascent / 2,
                        m_colorSubMenu.colors[i].first.c_str(),
                        (int)m_colorSubMenu.colors[i].first.length());
        }
    }
}

void Renderer::drawSizeSubMenuToBuffer(GC gc) {
    if (!m_sizeSubMenu.visible) return;

    int x = m_sizeSubMenu.x;
    int y = m_sizeSubMenu.y;
    int itemH = m_sizeSubMenu.itemHeight;
    int itemW = m_sizeSubMenu.itemWidth;
    int n = (int)m_sizeSubMenu.sizes.size();

    // White background with a thin gray border — matches the
    // new white toolbar look.
    setGCColor(m_display, gc, 255, 255, 255);
    XFillRectangle(m_display, m_buffer, gc, x, y, itemW, itemH * n);

    setGCColor(m_display, gc, 200, 200, 200);
    XDrawRectangle(m_display, m_buffer, gc, x, y, itemW, itemH * n);

    XFontStruct* sm = getSubmenuFont();
    if (sm) {
        // 10x10 filled black circle (radio marker) at the
        // left of the row. The text is offset to the right
        // of the circle so the two don't overlap.
        const int dotSize = 10;
        const int dotPad  = 8;
        const int dotX0   = x + dotPad;             // left edge of circle
        const int textX0  = dotX0 + dotSize + 8;    // text starts after circle

        for (int i = 0; i < n; i++) {
            bool hovered  = (i == m_sizeSubMenu.hoveredIndex);
            bool selected = (m_sizeSubMenu.sizes[i].second == m_fontSize);
            int itemY = y + i * itemH;

            // Hover row gets a light gray fill. The
            // selected indicator is drawn *on top* of this
            // fill, so the dot stays visible.
            if (hovered) {
                setGCColor(m_display, gc, 230, 230, 230);
                XFillRectangle(m_display, m_buffer, gc, x + 1, itemY + 1, itemW - 2, itemH - 2);
            }

            // Radio button: filled black circle when this
            // row is the current size, empty white circle
            // (just an outline) otherwise. XFillArc draws a
            // pie slice; passing 0..360*64 fills a full
            // disk. The Y axis is inverted vs normal math
            // (origin at top), so "center - r" is the top
            // of the circle in screen coordinates.
            int dotCy = itemY + itemH / 2;
            if (selected) {
                setGCColor(m_display, gc, 0, 0, 0);
                XFillArc(m_display, m_buffer, gc,
                         dotX0, dotCy - dotSize / 2,
                         dotSize, dotSize, 0, 360 * 64);
            } else {
                // Empty circle (white inside, black border)
                // so the rows stay visually consistent and
                // the selected one is the obvious "active"
                // option.
                setGCColor(m_display, gc, 255, 255, 255);
                XFillArc(m_display, m_buffer, gc,
                         dotX0, dotCy - dotSize / 2,
                         dotSize, dotSize, 0, 360 * 64);
                setGCColor(m_display, gc, 180, 180, 180);
                XSetLineAttributes(m_display, gc, 1,
                                   LineSolid, CapButt, JoinMiter);
                XDrawArc(m_display, m_buffer, gc,
                         dotX0, dotCy - dotSize / 2,
                         dotSize, dotSize, 0, 360 * 64);
            }

            // Label — fixed-size font so the menu doesn't
            // shift when the user changes the active text
            // size.
            setGCColor(m_display, gc, 0, 0, 0);
            XSetFont(m_display, gc, sm->fid);
            XDrawString(m_display, m_buffer, gc,
                        textX0, itemY + itemH / 2 + sm->ascent / 2,
                        m_sizeSubMenu.sizes[i].first.c_str(),
                        (int)m_sizeSubMenu.sizes[i].first.length());
        }
    }
}

void Renderer::drawTempShapeToBuffer(GC gc) {
    if (m_drawState != DrawState::DRAWING) return;

    if (m_toolMode == ToolMode::RECT || m_toolMode == ToolMode::ELLIPSE) {
        int x = std::min(m_tempX1, m_tempX2);
        int y = std::min(m_tempY1, m_tempY2);
        int w = std::abs(m_tempX2 - m_tempX1);
        int h = std::abs(m_tempY2 - m_tempY1);

        if (m_toolMode == ToolMode::RECT) {
            drawRect(m_buffer, gc, x, y, w, h, m_currentColor);
        } else {
            drawEllipse(m_buffer, gc, x + w/2, y + h/2, w/2, h/2, m_currentColor);
        }
    }
}

void Renderer::drawArrowsToBuffer(GC gc, const std::vector<Arrow>& arrows) {
    for (const auto& arrow : arrows) {
        drawArrowLine(m_buffer, gc, arrow.x1, arrow.y1, arrow.x2, arrow.y2, arrow.color);
    }

    if (m_drawingArrow && m_toolMode == ToolMode::ARROW) {
        drawArrowLine(m_buffer, gc, m_tempX1, m_tempY1, m_tempX2, m_tempY2, m_currentColor);
    }
}

void Renderer::drawArrowLine(Drawable d, GC gc, int x1, int y1, int x2, int y2, DrawColor color) {
    setGCColor(m_display, gc, color.r, color.g, color.b);
    XSetLineAttributes(m_display, gc, 2, LineSolid, CapRound, JoinRound);

    XDrawLine(m_display, d, gc, x1, y1, x2, y2);

    const double arrowHeadLength = 15;
    const double arrowHeadAngle = 0.5;
    double angle = atan2(y2 - y1, x2 - x1);
    int ax1 = static_cast<int>(x2 - arrowHeadLength * cos(angle - arrowHeadAngle));
    int ay1 = static_cast<int>(y2 - arrowHeadLength * sin(angle - arrowHeadAngle));
    int ax2 = static_cast<int>(x2 - arrowHeadLength * cos(angle + arrowHeadAngle));
    int ay2 = static_cast<int>(y2 - arrowHeadLength * sin(angle + arrowHeadAngle));

    XDrawLine(m_display, d, gc, x2, y2, ax1, ay1);
    XDrawLine(m_display, d, gc, x2, y2, ax2, ay2);
}

void Renderer::drawRectanglesToBuffer(GC gc, const std::vector<Rectangle>& rects) {
    for (const auto& rect : rects) {
        drawRect(m_buffer, gc, rect.x, rect.y, rect.w, rect.h, rect.color);
    }
}

void Renderer::drawRect(Drawable d, GC gc, int x, int y, int w, int h, DrawColor color) {
    setGCColor(m_display, gc, color.r, color.g, color.b);
    XSetLineAttributes(m_display, gc, 2, LineSolid, CapButt, JoinMiter);
    XDrawRectangle(m_display, d, gc, x, y, w, h);
}

void Renderer::drawEllipsesToBuffer(GC gc, const std::vector<Ellipse>& ellipses) {
    for (const auto& ellipse : ellipses) {
        drawEllipse(m_buffer, gc, ellipse.cx, ellipse.cy, ellipse.rx, ellipse.ry, ellipse.color);
    }
}

void Renderer::drawEllipse(Drawable d, GC gc, int cx, int cy, int rx, int ry, DrawColor color) {
    if (rx <= 0 || ry <= 0) return;

    setGCColor(m_display, gc, color.r, color.g, color.b);
    XSetLineAttributes(m_display, gc, 2, LineSolid, CapButt, JoinMiter);

    XDrawArc(m_display, d, gc, cx - rx, cy - ry, rx * 2, ry * 2, 0, 360 * 64);
}

void Renderer::drawTextsToBuffer(GC gc, const std::vector<TextAnnotation>& texts) {
    for (const auto& text : texts) {
        // Each annotation carries its own size, so the
        // rendered text never changes when the user toggles
        // the Size submenu.
        drawTextString(m_buffer, gc, text.x, text.y, text.text, text.color, text.fontSize);
    }
}

void Renderer::drawTextString(Drawable d, GC gc, int x, int y, const std::string& text, DrawColor color, int fontSize) {
    if (text.empty()) return;

    // Each piece of text remembers its own font size, so
    // changing the Size submenu does not retroactively
    // resize already-committed text. Look up the matching
    // font from the cache; fall back to the active font if
    // the size-specific one could not be loaded.
    XFontStruct* font = getFontForSize(fontSize);
    if (!font) font = m_font;
    if (!font) return;

    // text.y is the *baseline* of the string, not a
    // bounding-box coordinate. confirmText() computes it
    // so the committed text lines up exactly with what the
    // user saw in the input preview. XDrawString takes the
    // baseline as its y argument, so we just pass y through.

    // Subtle drop shadow for legibility on light backgrounds.
    setGCColor(m_display, gc, 0, 0, 0);
    XSetFont(m_display, gc, font->fid);
    XDrawString(m_display, d, gc, x + 1, y + 1,
                text.c_str(), (int)text.length());
    setGCColor(m_display, gc, color.r, color.g, color.b);
    XDrawString(m_display, d, gc, x, y,
                text.c_str(), (int)text.length());
}

void Renderer::drawTextInputBoxToBuffer() {
    if (!m_textInput) return;

    // Make sure the active bitmap font is loaded (set by
    // setFontSize, which runs on construction and on every
    // submenu pick). The fontset is still kept around for
    // CJK IME input — see below.
    ensureFallbackFont();

    // Width measurement: prefer the fontset so multibyte / CJK
    // strings report the correct visual width, and fall back to
    // the bitmap font for ASCII. We must measure the SAME way
    // we render below, otherwise the cursor drifts.
    int textWidth = 0;
    bool hasNonAscii = false;
    for (unsigned char c : m_currentInput) {
        if (c >= 0x80) { hasNonAscii = true; break; }
    }
    if (hasNonAscii && m_fontset) {
        XRectangle ink, logical;
        XmbTextExtents(m_fontset, m_currentInput.c_str(),
                       (int)m_currentInput.length(), &ink, &logical);
        textWidth = logical.width;
    } else if (m_font) {
        textWidth = XTextWidth(m_font, m_currentInput.c_str(), (int)m_currentInput.length());
    }

    int boxWidth = textWidth + 20;

    // Box height follows the active font's metrics. Use the
    // fontset's logical extent for CJK (it's reliable and
    // size-aware) and fall back to the bitmap font's
    // ascent/descent.
    int boxHeight = 24;
    if (m_fontset) {
        XRectangle ink, logical;
        XmbTextExtents(m_fontset, "Mg", 2, &ink, &logical);
        boxHeight = logical.height + 8;
    } else if (m_font) {
        boxHeight = m_font->ascent + m_font->descent + 8;
    }

    int boxY = m_textInputY - boxHeight / 2;

    GC inputGC = XCreateGC(m_display, m_buffer, 0, nullptr);

    setGCColor(m_display, inputGC, 255, 255, 255);
    XFillRectangle(m_display, m_buffer, inputGC, m_textInputX, boxY, boxWidth, boxHeight);

    setGCColor(m_display, inputGC, 0, 122, 255);
    XSetLineAttributes(m_display, inputGC, 2, LineSolid, CapButt, JoinMiter);
    XDrawRectangle(m_display, m_buffer, inputGC, m_textInputX, boxY, boxWidth, boxHeight);

    if (!m_currentInput.empty()) {
        // Render using the fontset for CJK (multibyte), the
        // size-aware bitmap font for Latin. This matches the
        // size the user picked: small uses 9x15, medium uses
        // 12x24, big uses 16x32.
        setGCColor(m_display, inputGC, m_currentColor.r, m_currentColor.g, m_currentColor.b);
        if (hasNonAscii && m_fontset) {
            XmbDrawString(m_display, m_buffer, m_fontset, inputGC,
                          m_textInputX + 4, boxY + boxHeight - 4,
                          m_currentInput.c_str(), (int)m_currentInput.length());
        } else if (m_font) {
            XSetFont(m_display, inputGC, m_font->fid);
            // Baseline near the bottom of the box, with 4px
            // above for descenders and the box border.
            int textY = boxY + boxHeight - 4 - m_font->descent;
            XDrawString(m_display, m_buffer, inputGC,
                        m_textInputX + 4, textY,
                        m_currentInput.c_str(), (int)m_currentInput.length());
        }

        int cursorX = m_textInputX + 4 + textWidth;
        XDrawLine(m_display, m_buffer, inputGC, cursorX, boxY + 3,
                  cursorX, boxY + boxHeight - 4);
    }

    XFreeGC(m_display, inputGC);
}

void Renderer::setSelectionBox(int x, int y, int width, int height) {
    m_selX = x;
    m_selY = y;
    m_selWidth = width;
    m_selHeight = height;
    m_hasSelection = (width > 0 && height > 0);
}

void Renderer::getSelectionBox(int& x, int& y, int& width, int& height) const {
    x = m_selX;
    y = m_selY;
    width = m_selWidth;
    height = m_selHeight;
}

void Renderer::setCurrentPixelColor(PixelColor color) {
    m_currentPixel = color;
}

void Renderer::setShowRGBPanel(bool show) {
    m_showRGBPanel = show;
}

void Renderer::setMenuButtons(const std::vector<MenuButton>& buttons) {
    m_menuButtons = buttons;
}

int Renderer::getClickedButton(int x, int y) const {
    for (size_t i = 0; i < m_menuButtons.size(); ++i) {
        const auto& btn = m_menuButtons[i];
        if (x >= btn.x && x <= btn.x + btn.width &&
            y >= btn.y && y <= btn.y + btn.height) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool Renderer::isPointInSelection(int x, int y) const {
    return m_hasSelection &&
           x >= m_selX && x <= m_selX + m_selWidth &&
           y >= m_selY && y <= m_selY + m_selHeight;
}

void Renderer::startShape(int x, int y) {
    if (!isPointInSelection(x, y)) return;
    m_tempX1 = m_tempX2 = x;
    m_tempY1 = m_tempY2 = y;
    m_drawState = DrawState::DRAWING;
}

void Renderer::updateShape(int x, int y, bool shiftPressed) {
    if (m_drawState != DrawState::DRAWING) return;
    m_tempX2 = x;
    m_tempY2 = y;

    // If shift pressed, make square/circle
    if (shiftPressed) {
        int dx = m_tempX2 - m_tempX1;
        int dy = m_tempY2 - m_tempY1;
        int size = std::max(std::abs(dx), std::abs(dy));
        m_tempX2 = m_tempX1 + (dx >= 0 ? size : -size);
        m_tempY2 = m_tempY1 + (dy >= 0 ? size : -size);
    }
}

void Renderer::finishShape(bool shiftPressed) {
    if (m_drawState != DrawState::DRAWING) return;
    m_drawState = DrawState::IDLE;

    int x = std::min(m_tempX1, m_tempX2);
    int y = std::min(m_tempY1, m_tempY2);
    int w = std::abs(m_tempX2 - m_tempX1);
    int h = std::abs(m_tempY2 - m_tempY1);

    // Enforce square/circle if shift was pressed during drawing
    if (shiftPressed) {
        int size = std::max(w, h);
        w = h = size;
    }

    if (w < 5 || h < 5) return;

    if (m_toolMode == ToolMode::RECT) {
        Rectangle rect = {x, y, w, h, m_currentColor};
        m_rectangles.push_back(rect);
    } else if (m_toolMode == ToolMode::ELLIPSE) {
        Ellipse ellipse = {x + w/2, y + h/2, w/2, h/2, m_currentColor};
        m_ellipses.push_back(ellipse);
    }
    saveHistory();
}

void Renderer::startArrow(int x, int y) {
    if (!isPointInSelection(x, y)) return;
    m_tempX1 = m_tempX2 = x;
    m_tempY1 = m_tempY2 = y;
    m_drawingArrow = true;
}

void Renderer::updateArrow(int x, int y) {
    if (!m_drawingArrow) return;
    m_tempX2 = x;
    m_tempY2 = y;
}

void Renderer::finishArrow() {
    if (!m_drawingArrow) return;
    m_drawingArrow = false;

    if (std::abs(m_tempX2 - m_tempX1) > 5 || std::abs(m_tempY2 - m_tempY1) > 5) {
        Arrow arrow = {m_tempX1, m_tempY1, m_tempX2, m_tempY2, m_currentColor};
        m_arrows.push_back(arrow);
        saveHistory();
    }
}

void Renderer::startText(int x, int y) {
    if (!isPointInSelection(x, y)) return;
    m_textInputX = x;
    m_textInputY = y;
    m_textInput = true;
    m_currentInput.clear();
}

void Renderer::updateTextPosition(int x, int y) {
    m_textInputX = x;
    m_textInputY = y;
}

void Renderer::inputText(const char* str, int len) {
    if (str && len > 0) {
        m_currentInput.append(str, len);
    }
}

void Renderer::deleteChar() {
    if (m_currentInput.empty()) return;
    // Pop one UTF-8 codepoint, not just one byte. UTF-8 continuation bytes
    // have the pattern 10xxxxxx, so we keep popping until we hit a byte that
    // is not a continuation byte (i.e. the start of the codepoint).
    m_currentInput.pop_back();
    while (!m_currentInput.empty()) {
        unsigned char c = static_cast<unsigned char>(m_currentInput.back());
        if ((c & 0xC0) != 0x80) break;
        m_currentInput.pop_back();
    }
}

void Renderer::confirmText() {
    if (!m_textInput || m_currentInput.empty()) {
        m_textInput = false;
        return;
    }

    // Compute the same box metrics the input box preview
    // used, so the committed text baseline lines up exactly
    // with what the user saw while typing. Otherwise the
    // committed text would appear noticeably lower than the
    // preview (the previous bug: text.y was set to the box
    // bottom and drawTextString then added (ascent-descent)/2
    // on top, pushing the baseline 10-20px below the input).
    int boxHeight = 24;
    int descent = 0;
    if (m_font) {
        boxHeight = m_font->ascent + m_font->descent + 8;
        descent = m_font->descent;
    }
    // baselineY is what drawTextString now uses directly.
    int baselineY = m_textInputY + boxHeight / 2 - 4 - descent;

    TextAnnotation text;
    text.x = m_textInputX;
    text.y = baselineY;
    text.text = m_currentInput;
    text.color = m_currentColor;
    text.fontSize = m_fontSize;
    m_texts.push_back(text);

    m_textInput = false;
    m_currentInput.clear();
    saveHistory();
}

void Renderer::cancelText() {
    m_textInput = false;
    m_currentInput.clear();
}

void Renderer::saveHistory() {
    if (m_historyIndex < (int)m_history.size() - 1) {
        m_history.erase(m_history.begin() + m_historyIndex + 1, m_history.end());
    }

    HistoryState state;
    state.arrows = m_arrows;
    state.rectangles = m_rectangles;
    state.ellipses = m_ellipses;
    state.texts = m_texts;
    m_history.push_back(state);
    m_historyIndex++;

    const int maxHistory = 50;
    if (m_history.size() > maxHistory) {
        m_history.erase(m_history.begin());
        m_historyIndex--;
    }
}

void Renderer::undo() {
    if (m_historyIndex <= 0) return;

    m_historyIndex--;
    const HistoryState& state = m_history[m_historyIndex];
    m_arrows = state.arrows;
    m_rectangles = state.rectangles;
    m_ellipses = state.ellipses;
    m_texts = state.texts;
}

void Renderer::redo() {
    if (m_historyIndex >= (int)m_history.size() - 1) return;

    m_historyIndex++;
    const HistoryState& state = m_history[m_historyIndex];
    m_arrows = state.arrows;
    m_rectangles = state.rectangles;
    m_ellipses = state.ellipses;
    m_texts = state.texts;
}

void Renderer::addArrow(const Arrow& arrow) {
    m_arrows.push_back(arrow);
}

void Renderer::addText(const TextAnnotation& text) {
    m_texts.push_back(text);
}

void Renderer::addRectangle(const Rectangle& rect) {
    m_rectangles.push_back(rect);
}

void Renderer::addEllipse(const Ellipse& ellipse) {
    m_ellipses.push_back(ellipse);
}

void Renderer::clearAnnotations() {
    m_arrows.clear();
    m_rectangles.clear();
    m_ellipses.clear();
    m_texts.clear();
}

bool Renderer::isPointInMenuBar(int x, int y) const {
    for (const auto& btn : m_menuButtons) {
        if (x >= btn.x && x <= btn.x + btn.width &&
            y >= btn.y && y <= btn.y + btn.height) {
            return true;
        }
    }
    return false;
}