#ifndef SCREEN_CAPTURE_UI_HELPERS_H
#define SCREEN_CAPTURE_UI_HELPERS_H

#include "globals.h"
#include "renderer.h"

#include <X11/Xlib.h>
#include <vector>

// Menu labels in English. Indexed by MenuButton::label. Maintained in
// ui_helpers.cpp to keep the layout strings co-located with calcMenuButtons.
extern const char* MENU_LABELS[];
extern const int NUM_BUTTONS;

// Returns which edge/corner of the selection box the mouse is over, or
// ResizeEdge::NONE if not within the resize threshold. Used to decide which
// resize cursor to display.
ResizeEdge getResizeEdge(int mx, int my, int selX, int selY, int selW, int selH);

// Creates a system X cursor matching the given resize edge (e.g. diagonal
// arrows for corners, vertical bar for top/bottom, etc.).
Cursor createResizeCursor(Display* display, ResizeEdge edge);

// Recomputes the menu button rectangles. Position is anchored below the
// selection (or above if it would overflow the screen).
void calcMenuButtons(int selX, int selY, int selW, int selH, int screenH,
                     std::vector<MenuButton>& buttons);

// Returns the index of the menu button under (mx, my), or -1 if none.
int getButtonAt(const std::vector<MenuButton>& buttons, int mx, int my);

// Returns the index of the color swatch under (mx, my) in the color submenu,
// or -1 if the submenu is hidden or the mouse is outside it.
int getColorSubMenuIndex(const ColorSubMenu& sub, int mx, int my);

// Returns the index of the size row under (mx, my) in the size submenu,
// or -1 if the submenu is hidden or the mouse is outside it.
int getSizeSubMenuIndex(const SizeSubMenu& sub, int mx, int my);

#endif // SCREEN_CAPTURE_UI_HELPERS_H
