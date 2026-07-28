#include "ui_helpers.h"

#include <X11/cursorfont.h>
#include <cstring>

// Menu labels in English
const char* MENU_LABELS[] = {"Arrow", "Rect", "Ellipse", "Text", "Size", "Color", "Undo", "Redo", "Save", "Pin", "Cancel", "Confirm"};
const int NUM_BUTTONS = 12;

ResizeEdge getResizeEdge(int mx, int my, int selX, int selY, int selW, int selH) {
    const int edgeSize = 8;
    bool onLeft   = (mx >= selX - edgeSize && mx <= selX + edgeSize);
    bool onRight  = (mx >= selX + selW - edgeSize && mx <= selX + selW + edgeSize);
    bool onTop    = (my >= selY - edgeSize && my <= selY + edgeSize);
    bool onBottom = (my >= selY + selH - edgeSize && my <= selY + selH + edgeSize);

    if (onTop && onLeft)     return ResizeEdge::TOP_LEFT;
    if (onTop && onRight)    return ResizeEdge::TOP_RIGHT;
    if (onBottom && onLeft)  return ResizeEdge::BOTTOM_LEFT;
    if (onBottom && onRight) return ResizeEdge::BOTTOM_RIGHT;
    if (onLeft)              return ResizeEdge::LEFT;
    if (onRight)             return ResizeEdge::RIGHT;
    if (onTop)               return ResizeEdge::TOP;
    if (onBottom)            return ResizeEdge::BOTTOM;
    return ResizeEdge::NONE;
}

Cursor createResizeCursor(Display* display, ResizeEdge edge) {
    unsigned int shape;
    switch (edge) {
        case ResizeEdge::LEFT:        shape = XC_left_side; break;
        case ResizeEdge::RIGHT:       shape = XC_right_side; break;
        case ResizeEdge::TOP:         shape = XC_top_side; break;
        case ResizeEdge::BOTTOM:      shape = XC_bottom_side; break;
        case ResizeEdge::TOP_LEFT:    shape = XC_top_left_corner; break;
        case ResizeEdge::TOP_RIGHT:   shape = XC_top_right_corner; break;
        case ResizeEdge::BOTTOM_LEFT: shape = XC_bottom_left_corner; break;
        case ResizeEdge::BOTTOM_RIGHT:shape = XC_bottom_right_corner; break;
        default:                      shape = XC_left_ptr; break;
    }
    return XCreateFontCursor(display, shape);
}

void calcMenuButtons(int selX, int selY, int selW, int selH, int screenH,
                     std::vector<MenuButton>& buttons) {
    buttons.clear();
    const int btnSpacing = 4;
    const int btnHeight = 26;
    const int btnWidth = 50;
    const int barPadV = 4;
    const int charWidth = 12;  // Chinese chars need more width

    int totalBtnWidth = 0;
    for (int i = 0; i < NUM_BUTTONS; i++) {
        int w = btnWidth + static_cast<int>(strlen(MENU_LABELS[i])) * charWidth;
        totalBtnWidth += w + btnSpacing;
    }

    int barHeight = btnHeight + barPadV * 2;

    int menuX = selX + (selW - totalBtnWidth) / 2;
    int menuY = selY + selH + 20;

    if (menuY + barHeight > screenH - 10) {
        menuY = selY - barHeight - 20;
    }
    if (menuY < 10) menuY = 10;
    if (menuX < 10) menuX = 10;

    int btnX = menuX;
    for (int i = 0; i < NUM_BUTTONS; i++) {
        MenuButton btn;
        btn.label = MENU_LABELS[i];
        btn.width = btnWidth + static_cast<int>(strlen(MENU_LABELS[i])) * charWidth;
        btn.height = btnHeight;
        btn.x = btnX;
        btn.y = menuY;
        btn.hovered = false;
        btn.pressed = false;
        btn.isSubMenu = false;
        btn.parentIndex = -1;
        buttons.push_back(btn);
        btnX += btn.width + btnSpacing;
    }
}

int getButtonAt(const std::vector<MenuButton>& buttons, int mx, int my) {
    for (size_t i = 0; i < buttons.size(); i++) {
        const MenuButton& btn = buttons[i];
        if (mx >= btn.x && mx <= btn.x + btn.width &&
            my >= btn.y && my <= btn.y + btn.height) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int getColorSubMenuIndex(const ColorSubMenu& sub, int mx, int my) {
    if (!sub.visible) return -1;
    if (mx < sub.x || mx > sub.x + sub.itemWidth) return -1;
    if (my < sub.y || my > sub.y + sub.itemHeight * (int)sub.colors.size()) return -1;

    int idx = (my - sub.y) / sub.itemHeight;
    if (idx >= 0 && idx < (int)sub.colors.size()) {
        return idx;
    }
    return -1;
}
