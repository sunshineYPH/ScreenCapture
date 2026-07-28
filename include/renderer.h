#ifndef RENDERER_H
#define RENDERER_H

#include "capture.h"
#include <X11/Xlib.h>
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

enum class DrawState {
    IDLE,
    DRAWING
};

struct MenuButton {
    int x, y, width, height;
    std::string label;
    bool hovered;
    bool isSubMenu;  // true if this is a submenu item
    int parentIndex; // index of parent button if submenu
};

struct ColorSubMenu {
    int x, y;
    int itemHeight;
    int itemWidth;
    std::vector<std::pair<std::string, DrawColor>> colors;
    int hoveredIndex;
    bool visible;
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

struct TextAnnotation {
    int x, y;
    std::string text;
    DrawColor color;
    int fontSize;
};

class Renderer {
public:
    Renderer(Display* display, Window window, ScreenCapture* capture);
    ~Renderer();

    void initDoubleBuffer();
    void render();
    void renderToPixmap(Pixmap pixmap, int selX, int selY, int selW, int selH);

    void setSelectionBox(int x, int y, int width, int height);
    void getSelectionBox(int& x, int& y, int& width, int& height) const;

    void setCurrentPixelColor(PixelColor color);
    void setShowRGBPanel(bool show);
    void setMenuButtons(const std::vector<MenuButton>& buttons);
    int getClickedButton(int x, int y) const;
    const ColorSubMenu& getColorSubMenu() const { return m_colorSubMenu; }
    void setColorSubMenuVisible(bool visible) { m_colorSubMenu.visible = visible; }
    void updateColorSubMenu(int baseX, int baseY);
    void setColorSubMenuHover(int idx) { m_colorSubMenu.hoveredIndex = idx; }
    int getColorSubMenuHover() const { return m_colorSubMenu.hoveredIndex; }

    void setToolMode(ToolMode mode) { m_toolMode = mode; }
    ToolMode getToolMode() const { return m_toolMode; }
    bool isDrawingShape() const { return m_drawState == DrawState::DRAWING; }
    bool isPointInSelection(int x, int y) const;

    void setDrawColor(DrawColor color) { m_currentColor = color; }
    DrawColor getDrawColor() const { return m_currentColor; }
    void setFontSize(int size) { m_fontSize = size; }
    int getFontSize() const { return m_fontSize; }

    bool isTextInput() const { return m_textInput; }
    void setTextInput(bool input) { m_textInput = input; }
    void startText(int x, int y);
    void inputText(char c);
    void deleteChar();
    void confirmText();
    void cancelText();

    // Shape drawing
    void startShape(int x, int y);
    void updateShape(int x, int y, bool shiftPressed);
    void finishShape(bool shiftPressed);
    int getTempShapeX1() const { return m_tempX1; }
    int getTempShapeY1() const { return m_tempY1; }
    int getTempShapeX2() const { return m_tempX2; }
    int getTempShapeY2() const { return m_tempY2; }

    void confirmText();
    void cancelText();

    std::string getCurrentInput() const { return m_currentInput; }
    int getTextInputX() const { return m_textInputX; }
    int getTextInputY() const { return m_textInputY; }

    std::vector<Arrow> getArrows() const { return m_arrows; }
    std::vector<Rectangle> getRectangles() const { return m_rectangles; }
    std::vector<Ellipse> getEllipses() const { return m_ellipses; }
    std::vector<TextAnnotation> getTexts() const { return m_texts; }

    void startArrow(int x, int y);
    void updateArrow(int x, int y);
    void finishArrow();

private:
    Display* m_display;
    Window m_window;
    GC m_gc;
    ScreenCapture* m_capture;

    int m_selX, m_selY, m_selWidth, m_selHeight;
    bool m_hasSelection;
    PixelColor m_currentPixel;
    bool m_showRGBPanel;
    std::vector<MenuButton> m_menuButtons;
    ColorSubMenu m_colorSubMenu;
    ToolMode m_toolMode;
    DrawState m_drawState;
    DrawColor m_currentColor;
    int m_fontSize;
    int m_tempX1, m_tempY1, m_tempX2, m_tempY2;  // temp shape while drawing
    bool m_drawingArrow;
    bool m_textInput;
    int m_textInputX, m_textInputY;
    std::string m_currentInput;
    std::vector<Arrow> m_arrows;
    std::vector<Rectangle> m_rectangles;
    std::vector<Ellipse> m_ellipses;
    std::vector<TextAnnotation> m_texts;

    void drawGrayOverlayToBuffer(GC gc);
    void drawSelectionToBuffer(GC gc);
    void drawRGBPanelToBuffer(GC gc);
    void drawMenuBarToBuffer(GC gc);
    void drawColorSubMenuToBuffer(GC gc);
    void drawArrowsToBuffer(GC gc, const std::vector<Arrow>& arrows);
    void drawRectanglesToBuffer(GC gc, const std::vector<Rectangle>& rects);
    void drawEllipsesToBuffer(GC gc, const std::vector<Ellipse>& ellipses);
    void drawTextsToBuffer(GC gc, const std::vector<TextAnnotation>& texts);
    void drawTextInputBoxToBuffer(GC gc);
    void drawTempShapeToBuffer(GC gc);
    void drawArrowLine(GC gc, int x1, int y1, int x2, int y2, DrawColor color);
    void drawRect(GC gc, int x, int y, int w, int h, DrawColor color);
    void drawEllipse(GC gc, int cx, int cy, int rx, int ry, DrawColor color);
    void drawTextString(GC gc, int x, int y, const std::string& text, DrawColor color);

    Pixmap m_buffer;
};

#endif // RENDERER_H