#ifndef RENDERER_H
#define RENDERER_H

#include "capture.h"
#include <X11/Xlib.h>
#include <unordered_map>
#include <vector>
#include <string>

struct DrawColor {
    int r, g, b;
    const char* name;
};

extern const DrawColor AVAILABLE_COLORS[];
extern const int NUM_COLORS;

enum class ToolMode {
    NONE,
    ARROW,
    RECT,
    ELLIPSE,
    TEXT
};

struct MenuButton {
    int x, y, width, height;
    std::string label;
    bool hovered;
    bool pressed;
    bool isSubMenu;
    int parentIndex;
};

struct Arrow {
    int x1, y1, x2, y2;
    DrawColor color;
};

struct Rectangle {
    int x, y, w, h;
    DrawColor color;
};

struct Ellipse {
    int cx, cy, rx, ry;
    DrawColor color;
};

struct ColorSubMenu {
    bool visible;
    int x, y;
    int itemWidth;
    int itemHeight;
    int hoveredIndex;
    std::vector<std::pair<std::string, DrawColor>> colors;
};

// Sub-menu attached to the "Size" toolbar button. The user
// picks one of three preset font sizes (small/medium/large)
// for subsequent text annotations. Mirrors ColorSubMenu in
// shape so the rendering and hit-test code stay symmetric.
struct SizeSubMenu {
    bool visible;
    int x, y;
    int itemWidth;
    int itemHeight;
    int hoveredIndex;
    // The size in points (or whatever units the renderer
    // uses) paired with a display label.
    std::vector<std::pair<std::string, int>> sizes;
};

struct TextAnnotation {
    int x, y;
    std::string text;
    DrawColor color;
    int fontSize;
};

struct HistoryState {
    std::vector<Arrow> arrows;
    std::vector<Rectangle> rectangles;
    std::vector<Ellipse> ellipses;
    std::vector<TextAnnotation> texts;
};

class Renderer {
public:
    Renderer(Display* display, Window window, ScreenCapture* capture);
    ~Renderer();

    void initDoubleBuffer();
    void render();
    void renderToPixmap(Pixmap pixmap, int selX, int selY, int selW, int selH);

    // Builds the cached half-blended (semi-transparent gray) image from the
    // screen capture. Called once from initDoubleBuffer; the result is reused
    // on every render so we don't re-blend on every frame.
    void createDimmedImage();

    void setSelectionBox(int x, int y, int width, int height);
    void getSelectionBox(int& x, int& y, int& width, int& height) const;

    void setCurrentPixelColor(PixelColor color);
    void setShowRGBPanel(bool show);
    void setMenuButtons(const std::vector<MenuButton>& buttons);
    int getClickedButton(int x, int y) const;

    void setToolMode(ToolMode mode) { m_toolMode = mode; }
    ToolMode getToolMode() const { return m_toolMode; }
    bool isDrawingArrow() const { return m_drawingArrow; }
    bool isDrawingShape() const { return m_drawState == DrawState::DRAWING; }
    bool isPointInSelection(int x, int y) const;
    bool isPointInMenuBar(int x, int y) const;

    void addArrow(const Arrow& arrow);
    void addText(const TextAnnotation& text);
    void addRectangle(const Rectangle& rect);
    void addEllipse(const Ellipse& ellipse);
    void clearAnnotations();

    // Lazily load the single-byte 9x15 font used as a text fallback
    // when a particular button's icon failed to load. Safe to call
    // repeatedly; only the first call hits X.
    void ensureFallbackFont();

    void undo();
    void redo();
    bool canUndo() const { return m_historyIndex > 0; }
    bool canRedo() const { return m_historyIndex < (int)m_history.size() - 1; }

    void setDrawColor(DrawColor color) { m_currentColor = color; }
    DrawColor getDrawColor() const { return m_currentColor; }
    void setFontSize(int size) {
        m_fontSize = size;
        // Set the active font by looking it up in (and
        // populating) the size cache. Subsequent setFontSize
        // calls with the same size are a no-op (the cache hit
        // is free). m_font is what the input box and
        // active-tool indicators read.
        m_font = getFontForSize(size);
    }

    // Look up the bitmap font for a given pixel size,
    // loading it on first request and caching for reuse.
    // We map the requested size to a specific X core bitmap
    // font that exists on this system (see comments in
    // setFontSize). Once loaded, a font is reused for every
    // text annotation of that size — so flipping the Size
    // submenu back and forth is free, and committed text
    // drawn with the old size still renders with its
    // original font after a size change.
    XFontStruct* getFontForSize(int size);

    // Fixed submenu font (loaded once, used for the
    // "small/medium/big" labels and the color name labels in
    // their respective popups). Independent of m_font.
    XFontStruct* getSubmenuFont();

    int getFontSize() const { return m_fontSize; }

    bool isTextInput() const { return m_textInput; }
    void setTextInput(bool input) { m_textInput = input; }
    void startText(int x, int y);
    void updateTextPosition(int x, int y);
    // Append a UTF-8 (or single-byte) string to the in-progress text input.
    // Takes (ptr, len) rather than a single char so multibyte IME commits
    // (e.g. Chinese characters committed by XIM) can be appended whole.
    void inputText(const char* str, int len);
    void deleteChar();
    void confirmText();
    void cancelText();

    void startArrow(int x, int y);
    void updateArrow(int x, int y);
    void finishArrow();

    void startShape(int x, int y);
    void updateShape(int x, int y, bool shiftPressed);
    void finishShape(bool shiftPressed);

    std::string getCurrentInput() const { return m_currentInput; }
    int getTextInputX() const { return m_textInputX; }
    int getTextInputY() const { return m_textInputY; }

    std::vector<Arrow> getArrows() const { return m_arrows; }
    std::vector<TextAnnotation> getTexts() const { return m_texts; }
    std::vector<Rectangle> getRectangles() const { return m_rectangles; }
    std::vector<Ellipse> getEllipses() const { return m_ellipses; }

    ColorSubMenu getColorSubMenu() const { return m_colorSubMenu; }
    void setColorSubMenuVisible(bool visible) { m_colorSubMenu.visible = visible; }
    void setColorSubMenuHover(int index) { m_colorSubMenu.hoveredIndex = index; }
    int getColorSubMenuHover() const { return m_colorSubMenu.hoveredIndex; }
    void updateColorSubMenu(int baseX, int baseY);

    SizeSubMenu getSizeSubMenu() const { return m_sizeSubMenu; }
    void setSizeSubMenuVisible(bool visible) { m_sizeSubMenu.visible = visible; }
    void setSizeSubMenuHover(int index) { m_sizeSubMenu.hoveredIndex = index; }
    int getSizeSubMenuHover() const { return m_sizeSubMenu.hoveredIndex; }
    void updateSizeSubMenu(int baseX, int baseY);

enum class DrawState {
    IDLE,
    DRAWING
};

private:
    Display* m_display;
    Window m_window;
    GC m_gc;
    ScreenCapture* m_capture;

    GC m_bufferGC;
    GC m_winGC;
    // m_font is the *active* font (whatever the user just
    // picked from the Size submenu). It's used for the input
    // box and the size submenu active row. For committed
    // text and submenu labels, each piece of text remembers
    // its own size and we look up the matching font from
    // m_fontsBySize below — so changing the size doesn't
    // retroactively resize old text.
    XFontStruct* m_font;
    bool m_fontLoaded;
    // Cache of fonts keyed by pixel size. Each committed
    // text annotation carries its own fontSize, so when we
    // re-draw the scene we look up the font for that exact
    // size here. We never mutate m_font when reading from
    // this cache.
    std::unordered_map<int, XFontStruct*> m_fontsBySize;
    // Fixed font used for submenu labels ("small/medium/big",
    // color names). Intentionally NOT affected by the user's
    // size choice — the submenu is metadata, not content.
    XFontStruct* m_submenuFont;
    XFontSet m_fontset;         // multibyte fontset for IME/CJK text input
    bool m_fontsetLoaded;

    int m_selX, m_selY, m_selWidth, m_selHeight;
    bool m_hasSelection;
    PixelColor m_currentPixel;
    bool m_showRGBPanel;
    std::vector<MenuButton> m_menuButtons;
    ToolMode m_toolMode;
    DrawColor m_currentColor;
    int m_fontSize;
    bool m_drawingArrow;
    bool m_textInput;
    int m_textInputX, m_textInputY;
    std::string m_currentInput;
    std::vector<Arrow> m_arrows;
    std::vector<Rectangle> m_rectangles;
    std::vector<Ellipse> m_ellipses;
    std::vector<TextAnnotation> m_texts;

    DrawState m_drawState;
    int m_tempX1, m_tempY1, m_tempX2, m_tempY2;
    ColorSubMenu m_colorSubMenu;
    SizeSubMenu   m_sizeSubMenu;

    std::vector<HistoryState> m_history;
    int m_historyIndex;

    void drawGrayOverlayToBuffer(GC gc);
    void drawSelectionToBuffer(GC gc);
    void drawRGBPanelToBuffer(GC gc);
    void drawMenuBarToBuffer(GC gc);
    void drawColorSubMenuToBuffer(GC gc);
    void drawSizeSubMenuToBuffer(GC gc);
    void drawArrowsToBuffer(GC gc, const std::vector<Arrow>& arrows);
    void drawRectanglesToBuffer(GC gc, const std::vector<Rectangle>& rects);
    void drawEllipsesToBuffer(GC gc, const std::vector<Ellipse>& ellipses);
    void drawTextsToBuffer(GC gc, const std::vector<TextAnnotation>& texts);
    void drawTextInputBoxToBuffer();
    void drawTempShapeToBuffer(GC gc);
    void drawArrowLine(Drawable d, GC gc, int x1, int y1, int x2, int y2, DrawColor color);
    void drawRect(Drawable d, GC gc, int x, int y, int w, int h, DrawColor color);
    void drawEllipse(Drawable d, GC gc, int cx, int cy, int rx, int ry, DrawColor color);
    void drawTextString(Drawable d, GC gc, int x, int y, const std::string& text, DrawColor color, int fontSize);
    void saveHistory();

    Pixmap m_buffer;
    XImage* m_dimmedImage; // Cached half-blended copy of the screen capture
};

#endif // RENDERER_H