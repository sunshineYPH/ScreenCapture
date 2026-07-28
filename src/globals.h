#ifndef SCREEN_CAPTURE_GLOBALS_H
#define SCREEN_CAPTURE_GLOBALS_H

// High-level application state. SELECTION_DONE means a region is selected and
// the menu/toolbar is interactive; TOOL_MODE means one of the annotation tools
// (arrow/rect/ellipse/text) is currently armed; DRAWING_SELECTION means the
// user is dragging out a new region.
enum class AppState {
    DRAWING_SELECTION,
    SELECTION_DONE,
    TOOL_MODE
};

// Which edge/corner of the selection box the cursor is currently hovering
// over. NONE means the cursor is not near any edge and the default
// hand/arrow cursor applies.
enum class ResizeEdge {
    NONE,
    LEFT, RIGHT, TOP, BOTTOM,
    TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT
};

#endif // SCREEN_CAPTURE_GLOBALS_H
