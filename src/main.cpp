#include "capture.h"
#include "clipboard.h"
#include "globals.h"
#include "button_icons.h"
#include "pin_window.h"
#include "renderer.h"
#include "ui_helpers.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/keysymdef.h>
#include <algorithm>
#include <clocale>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <vector>

// Set to false by Confirm/Save/Pin/Cancel/Esc to leave the main event loop
// and start the clipboard-wait phase. Lives in this TU only.
static bool g_running = true;

// X Input Method / Input Context for IME support. Without these, Chinese/Japanese
// IMEs cannot deliver committed strings to us — XLookupString would only see the
// raw keysym and the committed multibyte text would be lost. The XIC is created
// with the overlay as its client/focus window so the IME knows where to send
// preedit/commit notifications.
static XIM g_im = nullptr;
static XIC g_ic = nullptr;

int main(int argc, char* argv[]) {
    // When invoked as "screen_capture --pin <ppm_path>", run as a pin window
    // process (created by pinToScreen) instead of the normal screenshot flow.
    if (argc >= 3 && strcmp(argv[1], "--pin") == 0) {
        return runPinWindow(argv[2]);
    }

    (void)argc; (void)argv;

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "Error: Cannot open X11 display" << std::endl;
        return 1;
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    int screenWidth = DisplayWidth(display, screen);
    int screenHeight = DisplayHeight(display, screen);

    ScreenCapture* capture = new ScreenCapture(display);
    if (!capture->captureFullScreen()) {
        std::cerr << "Error: Failed to capture screen" << std::endl;
        XCloseDisplay(display);
        delete capture;
        return 1;
    }

    Window overlay = XCreateSimpleWindow(display, root,
        0, 0, screenWidth, screenHeight, 0, 0, 0);

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    XChangeWindowAttributes(display, overlay, CWOverrideRedirect, &attrs);

    XSelectInput(display, overlay,
        ButtonPressMask | ButtonReleaseMask |
        Button1MotionMask | PointerMotionMask |
        KeyPressMask);

    XMapRaised(display, overlay);
    XFlush(display);
    usleep(100000);

    // Explicitly set keyboard focus to the overlay. Previously this was
    // achieved by XGrabKeyboard, but we removed that because it breaks
    // XIM preedit. An override-redirect window does NOT receive keyboard
    // focus automatically — without this call, the overlay has no focus
    // and never sees any KeyPress event.
    XSetInputFocus(display, overlay, RevertToPointerRoot, CurrentTime);

    // Initialize XIM (Input Method) and create an XIC (Input Context) so that
    // Chinese/Japanese/Korean IMEs can deliver committed strings. Must be done
    // after setlocale (so the locale module list is populated) and after the
    // overlay window exists (the XIC needs XNClientWindow/XNFocusWindow).
    //
    // XOpenIM can legitimately return nullptr when no IME is installed
    // (e.g. minimal container, no fcitx/ibus). We must not let that take
    // out ASCII input — the text-input branch in the KeyPress handler falls
    // back to XLookupString when g_ic is null.
    //
    // Note: we intentionally do NOT call XGrabKeyboard here. XGrabKeyboard
    // breaks XIM preedit in many IME configurations — the IME needs to be
    // able to pop up its own windows over our overlay, and the keyboard grab
    // blocks that. The overlay is fullscreen and override-redirect, so it
    // receives keyboard events naturally as long as it has focus.
    setlocale(LC_ALL, "");
    // XSetLocaleModifiers("") picks up $XMODIFIERS automatically; we also
    // try the conventional @im=ibus / @im=fcitx in case $XMODIFIERS is
    // unset, so we don't fail on systems that have the IME daemon but no
    // user-level env config.
    if (XSetLocaleModifiers("") == nullptr) {
        if (XSetLocaleModifiers("@im=ibus") == nullptr) {
            XSetLocaleModifiers("@im=fcitx");
        }
    }
    g_im = XOpenIM(display, nullptr, nullptr, nullptr);
    if (g_im) {
        g_ic = XCreateIC(g_im,
            XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
            XNClientWindow, overlay,
            XNFocusWindow, overlay,
            nullptr);
        if (g_ic) {
            XSetICFocus(g_ic);
        } else {
            std::cerr << "Warning: XCreateIC failed, IME input will not work" << std::endl;
        }
    } else {
        std::cerr << "Warning: XOpenIM failed, IME input will not work" << std::endl;
    }

    Renderer* renderer = new Renderer(display, overlay, capture);
    renderer->initDoubleBuffer();

    // Load the menu-button icon PNGs from ./assets/ (or fallback
    // search paths inside button_icons::init). Loaded once up front
    // and cached as 24-bit server-side Pixmaps so XCopyArea is the
    // only per-frame work. If the assets directory is missing or any
    // icon is broken, the menu bar falls back to text labels — see
    // Renderer::drawMenuBarToBuffer.
    // The assetsDir argument is no longer used by button_icons;
    // it now resolves paths from the binary's own directory.
    // The arg is kept in the API for source compatibility.
    button_icons::init(display, DefaultScreen(display), "", 32);

    AppState state = AppState::DRAWING_SELECTION;
    int selStartX = 0, selStartY = 0;
    int selX = 0, selY = 0, selW = 0, selH = 0;
    bool isDragging = false;
    bool needRender = true;
    bool selectionValid = false;
    std::vector<MenuButton> menuButtons;

    int activeToolButton = -1; // Track which tool button is active (-1 = none)

    // Resize state
    bool isResizing = false;
    ResizeEdge resizeEdge = ResizeEdge::NONE;
    ResizeEdge currentCursorEdge = ResizeEdge::NONE;
    int resizeStartX = 0, resizeStartY = 0;
    int resizeOrigSelX = 0, resizeOrigSelY = 0;
    int resizeOrigSelW = 0, resizeOrigSelH = 0;

    // Drag state
    bool isDraggingSelection = false;
    int dragStartX = 0, dragStartY = 0;
    int dragOrigSelX = 0, dragOrigSelY = 0;

    // Create resize cursors
    Cursor cursors[9]; // indexed by ResizeEdge
    for (int i = 0; i < 9; i++) {
        cursors[i] = createResizeCursor(display, static_cast<ResizeEdge>(i));
    }
    Cursor defaultCursor = XCreateFontCursor(display, XC_left_ptr);
    Cursor handCursor = XCreateFontCursor(display, XC_hand2);
    Cursor crossCursor = XCreateFontCursor(display, XC_crosshair);

    XEvent ev;

    // Initial render
    renderer->setSelectionBox(selX, selY, selW, selH);
    renderer->render();
    XFlush(display);

    while (g_running) {
        XNextEvent(display, &ev);

        // Hand every event to the XIM filter first. XFilterEvent returns
        // True when the input method has consumed the event (e.g. it is
        // building a preedit string and will deliver the committed text
        // via a separate event). Without this, Chinese IMEs under
        // XGrabKeyboard swallow printable characters: XLookupKeysym still
        // returns a keysym, but XLookupString returns len == 0 and our
        // text-input branch sees nothing to append.
        if (XFilterEvent(&ev, overlay)) continue;

            switch (ev.type) {
                case MotionNotify:
                    if (isResizing) {
                        int dx = ev.xmotion.x - resizeStartX;
                        int dy = ev.xmotion.y - resizeStartY;

                        switch (resizeEdge) {
                            case ResizeEdge::LEFT:
                                selX = resizeOrigSelX + dx;
                                selW = resizeOrigSelW - dx;
                                break;
                            case ResizeEdge::RIGHT:
                                selW = resizeOrigSelW + dx;
                                break;
                            case ResizeEdge::TOP:
                                selY = resizeOrigSelY + dy;
                                selH = resizeOrigSelH - dy;
                                break;
                            case ResizeEdge::BOTTOM:
                                selH = resizeOrigSelH + dy;
                                break;
                            case ResizeEdge::TOP_LEFT:
                                selX = resizeOrigSelX + dx;
                                selY = resizeOrigSelY + dy;
                                selW = resizeOrigSelW - dx;
                                selH = resizeOrigSelH - dy;
                                break;
                            case ResizeEdge::TOP_RIGHT:
                                selY = resizeOrigSelY + dy;
                                selW = resizeOrigSelW + dx;
                                selH = resizeOrigSelH - dy;
                                break;
                            case ResizeEdge::BOTTOM_LEFT:
                                selX = resizeOrigSelX + dx;
                                selW = resizeOrigSelW - dx;
                                selH = resizeOrigSelH + dy;
                                break;
                            case ResizeEdge::BOTTOM_RIGHT:
                                selW = resizeOrigSelW + dx;
                                selH = resizeOrigSelH + dy;
                                break;
                            default: break;
                        }

                        // Enforce minimum size
                        if (selW < 50) {
                            if (resizeEdge == ResizeEdge::LEFT || resizeEdge == ResizeEdge::TOP_LEFT || resizeEdge == ResizeEdge::BOTTOM_LEFT) {
                                selX = resizeOrigSelX + resizeOrigSelW - 50;
                            }
                            selW = 50;
                        }
                        if (selH < 50) {
                            if (resizeEdge == ResizeEdge::TOP || resizeEdge == ResizeEdge::TOP_LEFT || resizeEdge == ResizeEdge::TOP_RIGHT) {
                                selY = resizeOrigSelY + resizeOrigSelH - 50;
                            }
                            selH = 50;
                        }

                        // Clamp to screen bounds
                        if (selX < 0) { selW += selX; selX = 0; }
                        if (selY < 0) { selH += selY; selY = 0; }
                        if (selX + selW > screenWidth) selW = screenWidth - selX;
                        if (selY + selH > screenHeight) selH = screenHeight - selY;

                        needRender = true;
                    } else if (isDraggingSelection) {
                        int dx = ev.xmotion.x - dragStartX;
                        int dy = ev.xmotion.y - dragStartY;
                        selX = dragOrigSelX + dx;
                        selY = dragOrigSelY + dy;

                        if (selX < 0) selX = 0;
                        if (selY < 0) selY = 0;
                        if (selX + selW > screenWidth) selX = screenWidth - selW;
                        if (selY + selH > screenHeight) selY = screenHeight - selH;

                        needRender = true;
                    } else if (isDragging && state == AppState::DRAWING_SELECTION) {
                        int currentX = ev.xmotion.x;
                        int currentY = ev.xmotion.y;

                        selX = std::min(selStartX, currentX);
                        selY = std::min(selStartY, currentY);
                        selW = std::abs(currentX - selStartX);
                        selH = std::abs(currentY - selStartY);

                        needRender = true;
                    } else if (state == AppState::TOOL_MODE && renderer->isDrawingShape()) {
                        // Read Shift directly from the event's state field. We used
                        // to track it via KeyPress/KeyRelease, but fcitx/ibus swallow
                        // the Shift KeyRelease (to toggle CN/EN) so the tracked flag
                        // would get stuck at true and every subsequent shape was
                        // forced to a square/circle. The event's state field is
                        // always accurate at the moment of the event.
                        bool shift = (ev.xmotion.state & ShiftMask) != 0;
                        renderer->updateShape(ev.xmotion.x, ev.xmotion.y, shift);
                        needRender = true;
                    } else if (state == AppState::TOOL_MODE && renderer->isDrawingShape() == false &&
                               (renderer->getToolMode() == ToolMode::ARROW)) {
                        renderer->updateArrow(ev.xmotion.x, ev.xmotion.y);
                        needRender = true;
                    }

                    if (state == AppState::SELECTION_DONE && selectionValid) {
                        int mx = ev.xmotion.x;
                        int my = ev.xmotion.y;

                        if (mx >= selX && mx <= selX + selW && my >= selY && my <= selY + selH) {
                            PixelColor pixel = capture->getPixelColor(mx, my);
                            renderer->setCurrentPixelColor(pixel);
                            renderer->setShowRGBPanel(true);
                        } else {
                            renderer->setShowRGBPanel(false);
                        }
                        needRender = true;
                    }

                    // Update hover states and cursor
                    if (selectionValid && !isResizing && !isDraggingSelection) {
                        bool changed = false;

                        // Check if cursor is near selection border for resize
                        ResizeEdge edge = ResizeEdge::NONE;
                        if (state == AppState::SELECTION_DONE || state == AppState::TOOL_MODE) {
                            edge = getResizeEdge(ev.xmotion.x, ev.xmotion.y, selX, selY, selW, selH);
                        }

                        if (edge != currentCursorEdge) {
                                currentCursorEdge = edge;
                                if (edge != ResizeEdge::NONE) {
                                    XDefineCursor(display, overlay, cursors[static_cast<int>(edge)]);
                                } else {
                                    bool insideSelection = (ev.xmotion.x >= selX && ev.xmotion.x <= selX + selW &&
                                                            ev.xmotion.y >= selY && ev.xmotion.y <= selY + selH);
                                    if (state == AppState::SELECTION_DONE && insideSelection) {
                                        XDefineCursor(display, overlay, handCursor);
                                    } else if (state == AppState::TOOL_MODE && insideSelection) {
                                        // Cross cursor only while inside the selection;
                                        // outside it falls through to the default arrow.
                                        XDefineCursor(display, overlay, crossCursor);
                                    } else {
                                        XDefineCursor(display, overlay, defaultCursor);
                                    }
                                }
                            }

                        // Check main menu buttons
                        for (auto& btn : menuButtons) {
                            bool newHover = (ev.xmotion.x >= btn.x &&
                                           ev.xmotion.x <= btn.x + btn.width &&
                                           ev.xmotion.y >= btn.y &&
                                           ev.xmotion.y <= btn.y + btn.height);
                            if (btn.hovered != newHover) {
                                btn.hovered = newHover;
                                changed = true;
                            }
                        }

                        // Check color submenu
                        ColorSubMenu sub = renderer->getColorSubMenu();
                        if (sub.visible) {
                            int colorIdx = getColorSubMenuIndex(sub, ev.xmotion.x, ev.xmotion.y);
                            if (colorIdx != renderer->getColorSubMenuHover()) {
                                renderer->setColorSubMenuHover(colorIdx);
                                changed = true;
                            }
                        }

                        // Check size submenu — same hover-tracking
                        // pattern as the color submenu.
                        SizeSubMenu sizeSub = renderer->getSizeSubMenu();
                        if (sizeSub.visible) {
                            int sizeIdx = getSizeSubMenuIndex(sizeSub, ev.xmotion.x, ev.xmotion.y);
                            if (sizeIdx != renderer->getSizeSubMenuHover()) {
                                renderer->setSizeSubMenuHover(sizeIdx);
                                changed = true;
                            }
                        }

                        if (changed) needRender = true;
                    }
                    break;

                case ButtonPress:
                    if (ev.xbutton.button == Button1) {
                        int mx = ev.xbutton.x;
                        int my = ev.xbutton.y;

                        if (state == AppState::DRAWING_SELECTION) {
                            isDragging = true;
                            selStartX = mx;
                            selStartY = my;
                            selX = mx;
                            selY = my;
                            selW = 0;
                            selH = 0;
                            selectionValid = false;
                            menuButtons.clear();
                            renderer->setMenuButtons(menuButtons);
                            renderer->setColorSubMenuVisible(false);
                            renderer->setSizeSubMenuVisible(false);
                            needRender = true;
                        } else if (state == AppState::SELECTION_DONE || state == AppState::TOOL_MODE) {
                            // Check if clicking on border for resize
                            ResizeEdge edge = getResizeEdge(mx, my, selX, selY, selW, selH);
                            if (edge != ResizeEdge::NONE) {
                                isResizing = true;
                                resizeEdge = edge;
                                resizeStartX = mx;
                                resizeStartY = my;
                                resizeOrigSelX = selX;
                                resizeOrigSelY = selY;
                                resizeOrigSelW = selW;
                                resizeOrigSelH = selH;
                                needRender = true;
                                break;
                            }

                            // Check color submenu first
                            ColorSubMenu sub = renderer->getColorSubMenu();
                            if (sub.visible) {
                                int colorIdx = getColorSubMenuIndex(sub, mx, my);
                                if (colorIdx >= 0) {
                                    renderer->setDrawColor(sub.colors[colorIdx].second);
                                    renderer->setColorSubMenuVisible(false);
                                    needRender = true;
                                    break;
                                } else {
                                    renderer->setColorSubMenuVisible(false);
                                }
                            }

                            // Check size submenu first (mirrors the
                            // color-submenu pattern above). Picking a
                            // size sets the font size used by the Text
                            // tool and closes the submenu.
                            SizeSubMenu sizeSub = renderer->getSizeSubMenu();
                            if (sizeSub.visible) {
                                int sizeIdx = getSizeSubMenuIndex(sizeSub, mx, my);
                                if (sizeIdx >= 0) {
                                    renderer->setFontSize(sizeSub.sizes[sizeIdx].second);
                                    renderer->setSizeSubMenuVisible(false);
                                    needRender = true;
                                    break;
                                } else {
                                    renderer->setSizeSubMenuVisible(false);
                                }
                            }

                            // Check main menu button
                            int btnIdx = getButtonAt(menuButtons, mx, my);
                            if (btnIdx >= 0) {
                                std::string clickedLabel = MENU_LABELS[btnIdx];

                                // Tool buttons (Arrow, Rect, Ellipse, Text) keep pressed state
                                bool isToolButton = (clickedLabel == "Arrow" || clickedLabel == "Rect" ||
                                                    clickedLabel == "Ellipse" || clickedLabel == "Text");

                                if (isToolButton) {
                                    // Track active tool button
                                    activeToolButton = btnIdx;
                                    // Set clicked tool button as pressed
                                    menuButtons[btnIdx].pressed = true;
                                    renderer->setMenuButtons(menuButtons);
                                } else {
                                    // Non-tool buttons: show brief highlight
                                    menuButtons[btnIdx].pressed = true;
                                    renderer->setMenuButtons(menuButtons);
                                    renderer->render();
                                    XFlush(display);
                                }

                                if (clickedLabel == "Undo") {
                                    renderer->undo();
                                } else if (clickedLabel == "Redo") {
                                    renderer->redo();
                                } else if (clickedLabel == "Confirm") {
                                    copyToClipboard(display, capture, renderer, selX, selY, selW, selH);
                                    g_running = false;
                                } else if (clickedLabel == "Save") {
                                    if (saveScreenshot(display, capture, renderer, selX, selY, selW, selH)) {
                                        copyToClipboard(display, capture, renderer, selX, selY, selW, selH);
                                    }
                                    g_running = false;
                                } else if (clickedLabel == "Pin") {
                                    pinToScreen(display, renderer, selX, selY, selW, selH);
                                    g_running = false;
                                } else if (clickedLabel == "Cancel") {
                                    g_running = false;
                                } else if (clickedLabel == "Arrow") {
                                    state = AppState::TOOL_MODE;
                                    renderer->setToolMode(ToolMode::ARROW);
                                } else if (clickedLabel == "Rect") {
                                    state = AppState::TOOL_MODE;
                                    renderer->setToolMode(ToolMode::RECT);
                                } else if (clickedLabel == "Ellipse") {
                                    state = AppState::TOOL_MODE;
                                    renderer->setToolMode(ToolMode::ELLIPSE);
                                } else if (clickedLabel == "Text") {
                                    state = AppState::TOOL_MODE;
                                    renderer->setToolMode(ToolMode::TEXT);
                                } else if (clickedLabel == "Size") {
                                    // Toggle the size submenu below the
                                    // Size button. If already visible, hide
                                    // it (so clicking the same button again
                                    // closes the submenu).
                                    MenuButton& sizeBtn = menuButtons[btnIdx];
                                    renderer->updateSizeSubMenu(sizeBtn.x, sizeBtn.y + sizeBtn.height + 4);
                                    if (renderer->getSizeSubMenu().visible) {
                                        renderer->setSizeSubMenuVisible(false);
                                    } else {
                                        renderer->setSizeSubMenuVisible(true);
                                        renderer->setSizeSubMenuHover(-1);
                                    }
                                } else if (clickedLabel == "Color") {
                                    // Show color subm+enu below the button
                                    MenuButton& colorBtn = menuButtons[btnIdx];
                                    renderer->updateColorSubMenu(colorBtn.x, colorBtn.y + colorBtn.height + 4);
                                    renderer->setColorSubMenuVisible(true);
                                    renderer->setColorSubMenuHover(-1);
                                }

                                if (!isToolButton) {
                                    // Skip the highlight delay for exit-triggering buttons
                                    // (Confirm/Save/Pin/Cancel) so the program reaches the
                                    // clipboard event loop and the X server can start
                                    // processing the overlay unmap as quickly as possible.
                                    bool isExitButton = (clickedLabel == "Confirm" ||
                                                         clickedLabel == "Save" ||
                                                         clickedLabel == "Pin" ||
                                                         clickedLabel == "Cancel");
                                    if (!isExitButton) {
                                        usleep(100000); // 100ms
                                    }
                                    menuButtons[btnIdx].pressed = false;
                                    renderer->setMenuButtons(menuButtons);
                                }
                                needRender = true;
                            }
                            // Drag selection in SELECTION_DONE state
                            else if (state == AppState::SELECTION_DONE &&
                                     mx >= selX && mx <= selX + selW &&
                                     my >= selY && my <= selY + selH) {
                                isDraggingSelection = true;
                                dragStartX = mx;
                                dragStartY = my;
                                dragOrigSelX = selX;
                                dragOrigSelY = selY;
                                needRender = true;
                            }
                            // Click inside selection for drawing
                            else if (mx >= selX && mx <= selX + selW && my >= selY && my <= selY + selH) {
                                ToolMode mode = renderer->getToolMode();
                                if (mode == ToolMode::ARROW) {
                                    renderer->startArrow(mx, my);
                                    needRender = true;
                                } else if (mode == ToolMode::RECT || mode == ToolMode::ELLIPSE) {
                                    renderer->startShape(mx, my);
                                    needRender = true;
                                } else if (mode == ToolMode::TEXT) {
                                    if (!renderer->isTextInput()) {
                                        renderer->startText(mx, my);
                                        needRender = true;
                                    } else if (renderer->getCurrentInput().empty()) {
                                        renderer->updateTextPosition(mx, my);
                                        needRender = true;
                                    }
                                }
                            }
                        }
                    }
                    break;

                case ButtonRelease:
                    if (ev.xbutton.button == Button1) {
                        if (isResizing) {
                            isResizing = false;
                            // Reset cursor on release
                            currentCursorEdge = ResizeEdge::NONE;
                            XDefineCursor(display, overlay, defaultCursor);
                            needRender = true;
                        } else if (isDraggingSelection) {
                            isDraggingSelection = false;
                        } else if (isDragging && state == AppState::DRAWING_SELECTION) {
                            isDragging = false;

                            int endX = ev.xbutton.x;
                            int endY = ev.xbutton.y;

                            selX = std::min(selStartX, endX);
                            selY = std::min(selStartY, endY);
                            selW = std::abs(endX - selStartX);
                            selH = std::abs(endY - selStartY);

                            if (selW >= 50 && selH >= 50) {
                                selectionValid = true;
                                state = AppState::SELECTION_DONE;
                            } else {
                                selW = 0;
                                selH = 0;
                                selectionValid = false;
                            }
                            needRender = true;
                        } else if (state == AppState::TOOL_MODE) {
                            ToolMode mode = renderer->getToolMode();
                            if (mode == ToolMode::ARROW) {
                                renderer->finishArrow();
                            } else if (mode == ToolMode::RECT || mode == ToolMode::ELLIPSE) {
                                bool shift = (ev.xbutton.state & ShiftMask) != 0;
                                renderer->finishShape(shift);
                            }
                            needRender = true;
                        }
                    }
                    break;

                case KeyPress: {
                    KeySym key = XLookupKeysym(&ev.xkey, 0);
                    bool ctrlPressed = (ev.xkey.state & ControlMask) != 0;

                    if (key == XK_Escape) {
                        if (renderer->isTextInput()) {
                            renderer->cancelText();
                            needRender = true;
                        } else {
                            g_running = false;
                        }
                        break;
                    }

                    if (ctrlPressed && key == XK_z) {
                        renderer->undo();
                        needRender = true;
                        break;
                    }

                    if (ctrlPressed && key == XK_y) {
                        renderer->redo();
                        needRender = true;
                        break;
                    }

                    // Arrow keys: nudge the selection rectangle. 1px normally,
                    // 10px when Shift is held. Clamped to screen bounds.
                    if (state == AppState::SELECTION_DONE) {
                        bool shift = (ev.xkey.state & ShiftMask) != 0;
                        int step = shift ? 10 : 1;
                        int newX = selX, newY = selY;
                        if (key == XK_Left)       newX -= step;
                        else if (key == XK_Right) newX += step;
                        else if (key == XK_Up)    newY -= step;
                        else if (key == XK_Down)  newY += step;
                        if (newX != selX || newY != selY) {
                            if (newX < 0) newX = 0;
                            if (newY < 0) newY = 0;
                            if (newX + selW > screenWidth)  newX = screenWidth  - selW;
                            if (newY + selH > screenHeight) newY = screenHeight - selH;
                            if (newX < 0) newX = 0;
                            if (newY < 0) newY = 0;
                            if (newX != selX || newY != selY) {
                                selX = newX;
                                selY = newY;
                                renderer->setSelectionBox(selX, selY, selW, selH);
                                needRender = true;
                            }
                        }
                    }

                    if (state == AppState::TOOL_MODE && renderer->isTextInput()) {
                        if (key == XK_Return || key == XK_KP_Enter) {
                            renderer->confirmText();
                            needRender = true;
                        } else if (key == XK_BackSpace) {
                            renderer->deleteChar();
                            needRender = true;
                        } else {
                            // Use XmbLookupString (multibyte) so IME-committed
                            // Chinese/Japanese strings get through. XLookupString
                            // would only see the raw keysym and lose the bytes
                            // the IME injected via XIM protocol.
                            //
                            // Fall back to XLookupString when no XIC is available
                            // (XOpenIM failed — e.g. no IME installed). Without
                            // this fallback the text input becomes completely
                            // dead even for ASCII.
                            char buffer[64];
                            int len = 0;
                            if (g_ic) {
                                Status status;
                                len = XmbLookupString(g_ic, &ev.xkey, buffer,
                                                      sizeof(buffer) - 1, nullptr, &status);
                                if (status == XBufferOverflow) {
                                    // Should not happen with 64 bytes for normal
                                    // text, but bail safely.
                                    len = 0;
                                }
                            } else {
                                len = XLookupString(&ev.xkey, buffer,
                                                    sizeof(buffer) - 1, nullptr, nullptr);
                            }
                            if (len > 0) {
                                renderer->inputText(buffer, len);
                                needRender = true;
                            }
                        }
                    }
                    break;
                }

            }

            // Process remaining pending events without blocking
            while (XPending(display) > 0) {
                XNextEvent(display, &ev);

                // Same IM filter as the main loop — without it Chinese
                // IMEs silently swallow the printable characters in the
                // inner pump too.
                if (XFilterEvent(&ev, overlay)) continue;

                switch (ev.type) {
                    case MotionNotify:
                        if (isResizing) {
                            int dx2 = ev.xmotion.x - resizeStartX;
                            int dy2 = ev.xmotion.y - resizeStartY;

                            switch (resizeEdge) {
                                case ResizeEdge::LEFT:
                                    selX = resizeOrigSelX + dx2;
                                    selW = resizeOrigSelW - dx2;
                                    break;
                                case ResizeEdge::RIGHT:
                                    selW = resizeOrigSelW + dx2;
                                    break;
                                case ResizeEdge::TOP:
                                    selY = resizeOrigSelY + dy2;
                                    selH = resizeOrigSelH - dy2;
                                    break;
                                case ResizeEdge::BOTTOM:
                                    selH = resizeOrigSelH + dy2;
                                    break;
                                case ResizeEdge::TOP_LEFT:
                                    selX = resizeOrigSelX + dx2;
                                    selY = resizeOrigSelY + dy2;
                                    selW = resizeOrigSelW - dx2;
                                    selH = resizeOrigSelH - dy2;
                                    break;
                                case ResizeEdge::TOP_RIGHT:
                                    selY = resizeOrigSelY + dy2;
                                    selW = resizeOrigSelW + dx2;
                                    selH = resizeOrigSelH - dy2;
                                    break;
                                case ResizeEdge::BOTTOM_LEFT:
                                    selX = resizeOrigSelX + dx2;
                                    selW = resizeOrigSelW - dx2;
                                    selH = resizeOrigSelH + dy2;
                                    break;
                                case ResizeEdge::BOTTOM_RIGHT:
                                    selW = resizeOrigSelW + dx2;
                                    selH = resizeOrigSelH + dy2;
                                    break;
                                default: break;
                            }

                            if (selW < 50) {
                                if (resizeEdge == ResizeEdge::LEFT || resizeEdge == ResizeEdge::TOP_LEFT || resizeEdge == ResizeEdge::BOTTOM_LEFT) {
                                    selX = resizeOrigSelX + resizeOrigSelW - 50;
                                }
                                selW = 50;
                            }
                            if (selH < 50) {
                                if (resizeEdge == ResizeEdge::TOP || resizeEdge == ResizeEdge::TOP_LEFT || resizeEdge == ResizeEdge::TOP_RIGHT) {
                                    selY = resizeOrigSelY + resizeOrigSelH - 50;
                                }
                                selH = 50;
                            }

                            if (selX < 0) { selW += selX; selX = 0; }
                            if (selY < 0) { selH += selY; selY = 0; }
                            if (selX + selW > screenWidth) selW = screenWidth - selX;
                            if (selY + selH > screenHeight) selH = screenHeight - selY;

                            needRender = true;
                        } else if (isDraggingSelection) {
                            int dx2 = ev.xmotion.x - dragStartX;
                            int dy2 = ev.xmotion.y - dragStartY;
                            selX = dragOrigSelX + dx2;
                            selY = dragOrigSelY + dy2;
                            if (selX < 0) selX = 0;
                            if (selY < 0) selY = 0;
                            if (selX + selW > screenWidth) selX = screenWidth - selW;
                            if (selY + selH > screenHeight) selY = screenHeight - selH;
                            needRender = true;
                        } else if (isDragging && state == AppState::DRAWING_SELECTION) {
                            int currentX = ev.xmotion.x;
                            int currentY = ev.xmotion.y;
                            selX = std::min(selStartX, currentX);
                            selY = std::min(selStartY, currentY);
                            selW = std::abs(currentX - selStartX);
                            selH = std::abs(currentY - selStartY);
                            needRender = true;
                        } else if (state == AppState::TOOL_MODE && renderer->isDrawingShape()) {
                            bool shift = (ev.xmotion.state & ShiftMask) != 0;
                            renderer->updateShape(ev.xmotion.x, ev.xmotion.y, shift);
                            needRender = true;
                        } else if (state == AppState::TOOL_MODE && !renderer->isDrawingShape() &&
                                   (renderer->getToolMode() == ToolMode::ARROW)) {
                            renderer->updateArrow(ev.xmotion.x, ev.xmotion.y);
                            needRender = true;
                        }
                        if (state == AppState::SELECTION_DONE && selectionValid) {
                            int mx2 = ev.xmotion.x;
                            int my2 = ev.xmotion.y;
                            if (mx2 >= selX && mx2 <= selX + selW && my2 >= selY && my2 <= selY + selH) {
                                PixelColor pixel = capture->getPixelColor(mx2, my2);
                                renderer->setCurrentPixelColor(pixel);
                                renderer->setShowRGBPanel(true);
                            } else {
                                renderer->setShowRGBPanel(false);
                            }
                            needRender = true;
                        }
                        if (selectionValid && !isResizing && !isDraggingSelection) {
                            bool changed2 = false;
                            ResizeEdge edge2 = ResizeEdge::NONE;
                            if (state == AppState::SELECTION_DONE || state == AppState::TOOL_MODE) {
                                edge2 = getResizeEdge(ev.xmotion.x, ev.xmotion.y, selX, selY, selW, selH);
                            }
                            if (edge2 != currentCursorEdge) {
                                currentCursorEdge = edge2;
                                if (edge2 != ResizeEdge::NONE) {
                                    XDefineCursor(display, overlay, cursors[static_cast<int>(edge2)]);
                                } else {
                                    bool insideSelection = (ev.xmotion.x >= selX && ev.xmotion.x <= selX + selW &&
                                                            ev.xmotion.y >= selY && ev.xmotion.y <= selY + selH);
                                    if (state == AppState::SELECTION_DONE && insideSelection) {
                                        XDefineCursor(display, overlay, handCursor);
                                    } else if (state == AppState::TOOL_MODE && insideSelection) {
                                        // Same as main switch: cross only inside selection.
                                        XDefineCursor(display, overlay, crossCursor);
                                    } else {
                                        XDefineCursor(display, overlay, defaultCursor);
                                    }
                                }
                            }
                            for (auto& btn : menuButtons) {
                                bool newHover = (ev.xmotion.x >= btn.x && ev.xmotion.x <= btn.x + btn.width &&
                                               ev.xmotion.y >= btn.y && ev.xmotion.y <= btn.y + btn.height);
                                if (btn.hovered != newHover) { btn.hovered = newHover; changed2 = true; }
                            }
                            ColorSubMenu sub2 = renderer->getColorSubMenu();
                            if (sub2.visible) {
                                int colorIdx = getColorSubMenuIndex(sub2, ev.xmotion.x, ev.xmotion.y);
                                if (colorIdx != renderer->getColorSubMenuHover()) {
                                    renderer->setColorSubMenuHover(colorIdx);
                                    changed2 = true;
                                }
                            }
                            SizeSubMenu sizeSub2 = renderer->getSizeSubMenu();
                            if (sizeSub2.visible) {
                                int sizeIdx = getSizeSubMenuIndex(sizeSub2, ev.xmotion.x, ev.xmotion.y);
                                if (sizeIdx != renderer->getSizeSubMenuHover()) {
                                    renderer->setSizeSubMenuHover(sizeIdx);
                                    changed2 = true;
                                }
                            }
                            if (changed2) needRender = true;
                        }
                        break;

                    case ButtonPress:
                        if (ev.xbutton.button == Button1) {
                            int mx2 = ev.xbutton.x;
                            int my2 = ev.xbutton.y;
                            if (state == AppState::DRAWING_SELECTION) {
                                isDragging = true;
                                selStartX = mx2; selStartY = my2;
                                selX = mx2; selY = my2; selW = 0; selH = 0;
                                selectionValid = false;
                                menuButtons.clear();
                                renderer->setMenuButtons(menuButtons);
                                renderer->setColorSubMenuVisible(false);
                                renderer->setSizeSubMenuVisible(false);
                                needRender = true;
                            } else if (state == AppState::SELECTION_DONE || state == AppState::TOOL_MODE) {
                                ResizeEdge edge2 = getResizeEdge(mx2, my2, selX, selY, selW, selH);
                                if (edge2 != ResizeEdge::NONE) {
                                    isResizing = true;
                                    resizeEdge = edge2;
                                    resizeStartX = mx2; resizeStartY = my2;
                                    resizeOrigSelX = selX; resizeOrigSelY = selY;
                                    resizeOrigSelW = selW; resizeOrigSelH = selH;
                                    needRender = true;
                                    break;
                                }
                                ColorSubMenu sub2 = renderer->getColorSubMenu();
                                if (sub2.visible) {
                                    int colorIdx = getColorSubMenuIndex(sub2, mx2, my2);
                                    if (colorIdx >= 0) {
                                        renderer->setDrawColor(sub2.colors[colorIdx].second);
                                        renderer->setColorSubMenuVisible(false);
                                        needRender = true;
                                        break;
                                    } else { renderer->setColorSubMenuVisible(false); }
                                }
                                SizeSubMenu sizeSub2 = renderer->getSizeSubMenu();
                                if (sizeSub2.visible) {
                                    int sizeIdx = getSizeSubMenuIndex(sizeSub2, mx2, my2);
                                    if (sizeIdx >= 0) {
                                        renderer->setFontSize(sizeSub2.sizes[sizeIdx].second);
                                        renderer->setSizeSubMenuVisible(false);
                                        needRender = true;
                                        break;
                                    } else { renderer->setSizeSubMenuVisible(false); }
                                }
                                int btnIdx = getButtonAt(menuButtons, mx2, my2);
                                if (btnIdx >= 0) {
                                    std::string clickedLabel = MENU_LABELS[btnIdx];

                                    bool isToolButton = (clickedLabel == "Arrow" || clickedLabel == "Rect" ||
                                                        clickedLabel == "Ellipse" || clickedLabel == "Text");

                                    if (isToolButton) {
                                        // Track active tool button
                                        activeToolButton = btnIdx;
                                        // Set clicked tool button as pressed
                                        menuButtons[btnIdx].pressed = true;
                                        renderer->setMenuButtons(menuButtons);
                                    } else {
                                        menuButtons[btnIdx].pressed = true;
                                        renderer->setMenuButtons(menuButtons);
                                        renderer->render();
                                        XFlush(display);
                                    }

                                    if (clickedLabel == "Undo") { renderer->undo();
                                    } else if (clickedLabel == "Redo") { renderer->redo();
                                    } else if (clickedLabel == "Confirm") {
                                        copyToClipboard(display, capture, renderer, selX, selY, selW, selH);
                                        g_running = false;
                                    } else if (clickedLabel == "Save") {
                                        if (saveScreenshot(display, capture, renderer, selX, selY, selW, selH)) {
                                            copyToClipboard(display, capture, renderer, selX, selY, selW, selH);
                                        }
                                        g_running = false;
                                    } else if (clickedLabel == "Pin") {
                                        pinToScreen(display, renderer, selX, selY, selW, selH);
                                        g_running = false;
                                    } else if (clickedLabel == "Cancel") { g_running = false;
                                    } else if (clickedLabel == "Arrow") { state = AppState::TOOL_MODE; renderer->setToolMode(ToolMode::ARROW);
                                    } else if (clickedLabel == "Rect") { state = AppState::TOOL_MODE; renderer->setToolMode(ToolMode::RECT);
                                    } else if (clickedLabel == "Ellipse") { state = AppState::TOOL_MODE; renderer->setToolMode(ToolMode::ELLIPSE);
                                    } else if (clickedLabel == "Text") { state = AppState::TOOL_MODE; renderer->setToolMode(ToolMode::TEXT);
                                    } else if (clickedLabel == "Size") {
                                        // Toggle the size submenu (mirrors the
                                        // single-line code path above).
                                        MenuButton& sizeBtn = menuButtons[btnIdx];
                                        renderer->updateSizeSubMenu(sizeBtn.x, sizeBtn.y + sizeBtn.height + 4);
                                        if (renderer->getSizeSubMenu().visible) {
                                            renderer->setSizeSubMenuVisible(false);
                                        } else {
                                            renderer->setSizeSubMenuVisible(true);
                                            renderer->setSizeSubMenuHover(-1);
                                        }
                                    } else if (clickedLabel == "Color") {
                                        MenuButton& colorBtn = menuButtons[btnIdx];
                                        renderer->updateColorSubMenu(colorBtn.x, colorBtn.y + colorBtn.height + 4);
                                        renderer->setColorSubMenuVisible(true);
                                        renderer->setColorSubMenuHover(-1);
                                    }

                                    if (!isToolButton) {
                                        // See note above: skip highlight delay for
                                        // exit-triggering buttons.
                                        bool isExitButton = (clickedLabel == "Confirm" ||
                                                             clickedLabel == "Save" ||
                                                             clickedLabel == "Pin" ||
                                                             clickedLabel == "Cancel");
                                        if (!isExitButton) {
                                            usleep(100000);
                                        }
                                        menuButtons[btnIdx].pressed = false;
                                        renderer->setMenuButtons(menuButtons);
                                    }
                                    needRender = true;
                                }
                                // Drag selection in SELECTION_DONE state
                                else if (state == AppState::SELECTION_DONE &&
                                         mx2 >= selX && mx2 <= selX + selW &&
                                         my2 >= selY && my2 <= selY + selH) {
                                    isDraggingSelection = true;
                                    dragStartX = mx2;
                                    dragStartY = my2;
                                    dragOrigSelX = selX;
                                    dragOrigSelY = selY;
                                    needRender = true;
                                }
                                // Click inside selection for drawing
                                else if (mx2 >= selX && mx2 <= selX + selW && my2 >= selY && my2 <= selY + selH) {
                                    ToolMode mode = renderer->getToolMode();
                                    if (mode == ToolMode::ARROW) { renderer->startArrow(mx2, my2); needRender = true;
                                    } else if (mode == ToolMode::RECT || mode == ToolMode::ELLIPSE) { renderer->startShape(mx2, my2); needRender = true;
                                    } else if (mode == ToolMode::TEXT) { if (!renderer->isTextInput()) { renderer->startText(mx2, my2); needRender = true; } else if (renderer->getCurrentInput().empty()) { renderer->updateTextPosition(mx2, my2); needRender = true; } }
                                }
                            }
                        }
                        break;

                    case ButtonRelease:
                        if (ev.xbutton.button == Button1) {
                            if (isResizing) {
                                isResizing = false;
                                currentCursorEdge = ResizeEdge::NONE;
                                XDefineCursor(display, overlay, defaultCursor);
                                needRender = true;
                            } else if (isDraggingSelection) {
                                isDraggingSelection = false;
                            } else if (isDragging && state == AppState::DRAWING_SELECTION) {
                                isDragging = false;
                                int endX = ev.xbutton.x;
                                int endY = ev.xbutton.y;
                                selX = std::min(selStartX, endX);
                                selY = std::min(selStartY, endY);
                                selW = std::abs(endX - selStartX);
                                selH = std::abs(endY - selStartY);
                                if (selW >= 50 && selH >= 50) { selectionValid = true; state = AppState::SELECTION_DONE; }
                                else { selW = 0; selH = 0; selectionValid = false; }
                                needRender = true;
                            } else if (state == AppState::TOOL_MODE) {
                                ToolMode mode = renderer->getToolMode();
                                if (mode == ToolMode::ARROW) { renderer->finishArrow();
                                } else if (mode == ToolMode::RECT || mode == ToolMode::ELLIPSE) {
                                    bool shift = (ev.xbutton.state & ShiftMask) != 0;
                                    renderer->finishShape(shift);
                                }
                                needRender = true;
                            }
                        }
                        break;

                    case KeyPress: {
                        KeySym key = XLookupKeysym(&ev.xkey, 0);
                        bool ctrlPressed = (ev.xkey.state & ControlMask) != 0;
                        if (key == XK_Escape) {
                            if (renderer->isTextInput()) { renderer->cancelText(); needRender = true; }
                            else { g_running = false; }
                            break;
                        }
                        if (ctrlPressed && key == XK_z) { renderer->undo(); needRender = true; break; }
                        if (ctrlPressed && key == XK_y) { renderer->redo(); needRender = true; break; }
                        // Arrow keys: same nudge behavior as the main switch.
                        if (state == AppState::SELECTION_DONE) {
                            bool shift = (ev.xkey.state & ShiftMask) != 0;
                            int step = shift ? 10 : 1;
                            int newX = selX, newY = selY;
                            if (key == XK_Left)       newX -= step;
                            else if (key == XK_Right) newX += step;
                            else if (key == XK_Up)    newY -= step;
                            else if (key == XK_Down)  newY += step;
                            if (newX != selX || newY != selY) {
                                if (newX < 0) newX = 0;
                                if (newY < 0) newY = 0;
                                if (newX + selW > screenWidth)  newX = screenWidth  - selW;
                                if (newY + selH > screenHeight) newY = screenHeight - selH;
                                if (newX < 0) newX = 0;
                                if (newY < 0) newY = 0;
                                if (newX != selX || newY != selY) {
                                    selX = newX;
                                    selY = newY;
                                    renderer->setSelectionBox(selX, selY, selW, selH);
                                    needRender = true;
                                }
                            }
                        }
                        if (state == AppState::TOOL_MODE && renderer->isTextInput()) {
                            if (key == XK_Return || key == XK_KP_Enter) { renderer->confirmText(); needRender = true;
                            } else if (key == XK_BackSpace) { renderer->deleteChar(); needRender = true;
                            } else {
                                // Multibyte lookup with fallback to ASCII-only
                                // XLookupString when XIM is unavailable — same
                                // logic as the main switch.
                                char buffer[64];
                                int len = 0;
                                if (g_ic) {
                                    Status status;
                                    len = XmbLookupString(g_ic, &ev.xkey, buffer,
                                                          sizeof(buffer) - 1, nullptr, &status);
                                    if (status == XBufferOverflow) len = 0;
                                } else {
                                    len = XLookupString(&ev.xkey, buffer,
                                                        sizeof(buffer) - 1, nullptr, nullptr);
                                }
                                if (len > 0) { renderer->inputText(buffer, len); needRender = true; }
                            }
                        }
                        break;
                    }

                }
            }

            // After Confirm/Cancel/Save, g_running is false. Skip the render block —
            // the overlay is about to be unmapped and the clipboard event loop is
            // starting, so re-rendering the now-defunct overlay is wasted work.
            if (!g_running) break;

        if (needRender) {
            renderer->setSelectionBox(selX, selY, selW, selH);

            if (selectionValid) {
                calcMenuButtons(selX, selY, selW, selH, screenHeight, menuButtons);
                // Restore active tool button highlight
                if (activeToolButton >= 0 && activeToolButton < NUM_BUTTONS) {
                    std::string label = MENU_LABELS[activeToolButton];
                    if (label == "Arrow" || label == "Rect" ||
                        label == "Ellipse" || label == "Text") {
                        menuButtons[activeToolButton].pressed = true;
                    } else {
                        activeToolButton = -1; // Reset if not a tool button
                    }
                }
            } else {
                menuButtons.clear();
            }
            renderer->setMenuButtons(menuButtons);

            renderer->render();
            XFlush(display);
            needRender = false;
        }
    }

    // Release overlay BEFORE clipboard wait loop so the user can switch to
    // another app to paste. We do NOT call XUngrabKeyboard anymore — the
    // keyboard grab was removed at startup because it breaks IME preedit
    // under XGrabKeyboard (see the XIM init comment above).
    if (g_ic) {
        XUnsetICFocus(g_ic);
    }
    XUnmapWindow(display, overlay);
    // Non-blocking flush: let X server process unmap/expose/repaint in background
    // while we immediately start the clipboard event loop. XSync() here would block
    // for several seconds waiting for the server to finish repainting everything
    // underneath, delaying our response to SelectionRequest from the paste target.
    XFlush(display);

    Atom targetsAtom = XInternAtom(display, "TARGETS", False);
    Atom timestampAtom = XInternAtom(display, "TIMESTAMP", False);

    // Use select() with X11 file descriptor for responsive event handling
    int x11_fd = ConnectionNumber(display);
    fd_set in_fds;
    int timeout_counter = 0;
    const int MAX_TIMEOUT = 300; // 30 seconds (300 * 100ms)

    while (g_clipboardActive && timeout_counter < MAX_TIMEOUT) {
        // Process all pending events first
        while (XPending(display) > 0) {
            XEvent ev;
            XNextEvent(display, &ev);

            if (ev.type == SelectionRequest) {
                XSelectionRequestEvent* req = &ev.xselectionrequest;
                XSelectionEvent reply;
                reply.type = SelectionNotify;
                reply.requestor = req->requestor;
                reply.selection = req->selection;
                reply.target = req->target;
                reply.property = None;
                reply.time = req->time;

                if (req->target == targetsAtom) {
                    // Return list of supported targets
                    Atom supported[] = { g_clipboardPngAtom, timestampAtom };
                    reply.property = req->property;
                    if (reply.property == None) {
                        reply.property = XInternAtom(display, "CLIPBOARD_DATA", False);
                    }
                    XChangeProperty(display, req->requestor, reply.property,
                                    XA_ATOM, 32, PropModeReplace,
                                    reinterpret_cast<unsigned char*>(supported), 2);
                } else if (req->target == timestampAtom) {
                    // Return the timestamp when we acquired the selection
                    reply.property = req->property;
                    if (reply.property == None) {
                        reply.property = XInternAtom(display, "CLIPBOARD_DATA", False);
                    }
                    unsigned long ts = g_clipboardTimestamp;
                    XChangeProperty(display, req->requestor, reply.property,
                                    XA_INTEGER, 32, PropModeReplace,
                                    reinterpret_cast<unsigned char*>(&ts), 1);
                } else if (req->target == g_clipboardPngAtom) {
                    reply.property = req->property;
                    if (reply.property == None) {
                        reply.property = XInternAtom(display, "CLIPBOARD_DATA", False);
                    }
                    XChangeProperty(display, req->requestor, reply.property,
                                    g_clipboardPngAtom, 8, PropModeReplace,
                                    g_clipboardData.data(), g_clipboardData.size());
                    XSync(display, False);
                }
                XSendEvent(display, req->requestor, False, NoEventMask,
                           reinterpret_cast<XEvent*>(&reply));
                XFlush(display);

                // Exit immediately after sending PNG data
                if (req->target == g_clipboardPngAtom) {
                    g_clipboardActive = false;
                    break;
                }
            } else if (ev.type == SelectionClear) {
                // Another app took clipboard ownership, exit
                g_clipboardActive = false;
                break;
            }
        }

        if (!g_clipboardActive) break;

        // Wait for new events with 100ms timeout
        FD_ZERO(&in_fds);
        FD_SET(x11_fd, &in_fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms

        select(x11_fd + 1, &in_fds, nullptr, nullptr, &tv);
        timeout_counter++;
    }

    // Cleanup child process
    if (g_clipboardWin != None) {
        XDestroyWindow(display, g_clipboardWin);
    }

    // Tear down XIM/XIC before closing the display so the IME can release
    // any preedit state it was holding.
    if (g_ic) {
        XDestroyIC(g_ic);
        g_ic = nullptr;
    }
    if (g_im) {
        XCloseIM(g_im);
        g_im = nullptr;
    }

    // Release cached menu-icon Pixmaps BEFORE XCloseDisplay, since
    // cleanup needs a live Display*.
    button_icons::cleanup();

    XCloseDisplay(display);

    return 0;
}
