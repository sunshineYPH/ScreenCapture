#include "renderer.h"
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

    saveHistory();
}

Renderer::~Renderer() {
    if (m_gc) XFreeGC(m_display, m_gc);
    if (m_bufferGC) XFreeGC(m_display, m_bufferGC);
    if (m_winGC) XFreeGC(m_display, m_winGC);
    if (m_buffer != None) XFreePixmap(m_display, m_buffer);
    if (m_dimmedImage) XDestroyImage(m_dimmedImage);
    if (m_font) XFreeFont(m_display, m_font);
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
        drawTextString(pixmap, gc, tx, ty, text.text, text.color);
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
    const int btnHeight = 26;
    const int barPadH = 10;
    const int barPadV = 4;

    for (const auto& btn : m_menuButtons) {
        totalWidth += btn.width + btnSpacing;
    }

    int barX = m_menuButtons[0].x - barPadH;
    int barY = m_menuButtons[0].y - barPadV;
    int barWidth = totalWidth + barPadH * 2;
    int barHeight = btnHeight + barPadV * 2;

    setGCColor(m_display, gc, 45, 45, 45);
    XFillRectangle(m_display, m_buffer, gc, barX, barY, barWidth, barHeight);

    setGCColor(m_display, gc, 70, 70, 70);
    XDrawRectangle(m_display, m_buffer, gc, barX, barY, barWidth, barHeight);

    if (!m_fontLoaded) {
        m_font = XLoadQueryFont(m_display, "9x15");
        m_fontLoaded = true;
    }
    if (m_font) {
        for (const auto& btn : m_menuButtons) {
            if (btn.pressed) {
                setGCColor(m_display, gc, 100, 100, 100);
            } else if (btn.hovered) {
                setGCColor(m_display, gc, 65, 65, 65);
            } else {
                setGCColor(m_display, gc, 55, 55, 55);
            }
            XFillRectangle(m_display, m_buffer, gc, btn.x, btn.y, btn.width, btn.height);

            setGCColor(m_display, gc, 90, 90, 90);
            XDrawRectangle(m_display, m_buffer, gc, btn.x, btn.y, btn.width, btn.height);

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
    if (m_font) {
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
            XSetFont(m_display, gc, m_font->fid);
            XDrawString(m_display, m_buffer, gc, x + 24, itemY + itemH / 2 + m_font->ascent / 2,
                        m_colorSubMenu.colors[i].first.c_str(),
                        (int)m_colorSubMenu.colors[i].first.length());
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
        drawTextString(m_buffer, gc, text.x, text.y, text.text, text.color);
    }
}

void Renderer::drawTextString(Drawable d, GC gc, int x, int y, const std::string& text, DrawColor color) {
    if (text.empty()) return;

    if (!m_fontLoaded) {
        m_font = XLoadQueryFont(m_display, "9x15");
        if (!m_font) m_font = XLoadQueryFont(m_display, "fixed");
        m_fontLoaded = true;
    }

    if (m_font) {
        int baselineY = y - 4;
        setGCColor(m_display, gc, 0, 0, 0);
        XSetFont(m_display, gc, m_font->fid);
        XDrawString(m_display, d, gc, x + 1, baselineY + 1, text.c_str(), (int)text.length());

        setGCColor(m_display, gc, color.r, color.g, color.b);
        XDrawString(m_display, d, gc, x, baselineY, text.c_str(), (int)text.length());
    }
}

void Renderer::drawTextInputBoxToBuffer() {
    if (!m_textInput) return;

    // Load a multibyte fontset on first use (or after the size changed).
    // The pattern includes the current m_fontSize so picking a different
    // size from the menu actually resizes the input text. We try a
    // specific XLFD pattern first, then fall back to "*" which lets Xlib
    // pick a font matching the current locale (so CJK characters committed
    // by the IME actually render instead of showing up as tofu boxes).
    if (!m_fontsetLoaded) {
        char pattern[256];
        // XLFD field 7 is pixel size. Wildcards everywhere else let the
        // X server choose a font that supports the current locale.
        snprintf(pattern, sizeof(pattern),
                 "-misc-fixed-*-r-normal--*-%d-*-*-*-*-*-*-*",
                 m_fontSize);
        char** missing_charsets = nullptr;
        int num_missing = 0;
        m_fontset = XCreateFontSet(m_display, pattern,
                                   &missing_charsets, &num_missing, nullptr);
        if (!m_fontset) {
            // Drop the size constraint and let the server pick.
            m_fontset = XCreateFontSet(m_display, "-*-*-*-*-*-*-*-*-*-*-*-*-*-*",
                                       &missing_charsets, &num_missing, nullptr);
        }
        if (!m_fontset) {
            m_fontset = XCreateFontSet(m_display, "*",
                                       &missing_charsets, &num_missing, nullptr);
        }
        if (missing_charsets) XFreeStringList(missing_charsets);
        m_fontsetLoaded = true;
    }

    // Also keep the legacy single-byte font loaded so XTextWidth keeps
    // working for width measurement. XTextWidth on a multibyte string
    // returns the byte count, not the visual width, so it underestimates
    // CJK strings — we add a per-byte pad to compensate.
    if (!m_fontLoaded) {
        m_font = XLoadQueryFont(m_display, "9x15");
        if (!m_font) m_font = XLoadQueryFont(m_display, "fixed");
        m_fontLoaded = true;
    }

    // Measure the visual width of the current input. XTextWidth is wrong for
    // multibyte text (it counts bytes, not glyphs), so prefer XmbTextExtents
    // when the fontset is available. We must use the SAME measurement for
    // positioning the cursor, otherwise the cursor drifts to ~2x the actual
    // text end (the previous bug from a per-byte pad on top of XTextWidth).
    int textWidth = 0;
    if (m_fontset) {
        XRectangle ink, logical;
        XmbTextExtents(m_fontset, m_currentInput.c_str(),
                       (int)m_currentInput.length(), &ink, &logical);
        textWidth = logical.width;
    } else if (m_font) {
        textWidth = XTextWidth(m_font, m_currentInput.c_str(), (int)m_currentInput.length());
    }
    // Small fixed padding so the cursor / box border don't touch the glyphs.
    int boxWidth = textWidth + 20;
    int boxHeight = 24;
    if (m_font) boxHeight = m_font->ascent + m_font->descent + 8;

    int boxY = m_textInputY - boxHeight / 2;

    GC inputGC = XCreateGC(m_display, m_buffer, 0, nullptr);

    setGCColor(m_display, inputGC, 255, 255, 255);
    XFillRectangle(m_display, m_buffer, inputGC, m_textInputX, boxY, boxWidth, boxHeight);

    setGCColor(m_display, inputGC, 0, 122, 255);
    XSetLineAttributes(m_display, inputGC, 2, LineSolid, CapButt, JoinMiter);
    XDrawRectangle(m_display, m_buffer, inputGC, m_textInputX, boxY, boxWidth, boxHeight);

    if (!m_currentInput.empty()) {
        // Use the active color from the Color menu. We MUST go through
        // setGCColor / XAllocColor — direct XSetForeground with a manual
        // bit shift gives the wrong pixel because X's default visual
        // is not 24-bit truecolor; XAllocColor maps the requested RGB
        // into the colormap's actual pixel value.
        if (m_fontset) {
            // XmbDrawString renders multibyte strings using the fontset —
            // this is what makes Chinese characters appear correctly.
            setGCColor(m_display, inputGC, m_currentColor.r, m_currentColor.g, m_currentColor.b);
            XmbDrawString(m_display, m_buffer, m_fontset, inputGC,
                          m_textInputX + 4, boxY + boxHeight - 4,
                          m_currentInput.c_str(), (int)m_currentInput.length());
        } else if (m_font) {
            // Last-resort ASCII fallback.
            setGCColor(m_display, inputGC, m_currentColor.r, m_currentColor.g, m_currentColor.b);
            XSetFont(m_display, inputGC, m_font->fid);
            XDrawString(m_display, m_buffer, inputGC, m_textInputX + 4, boxY + boxHeight - 4,
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

    int boxHeight = 24;
    if (m_font) boxHeight = m_font->ascent + m_font->descent + 8;

    TextAnnotation text;
    text.x = m_textInputX;
    text.y = m_textInputY + boxHeight / 2;
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