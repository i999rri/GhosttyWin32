#pragma once

#include "ghostty.h"
#include <windows.h>

namespace core::ghostty::actions::tags {

// CELL_SIZE action value (the cell's pixel dimensions) plus the
// WM_SIZING subclass that snaps interactive window resizing to the
// cell grid — the Win32 counterpart of macOS's
// contentResizeIncrements (#155). Named after the ghostty action
// tag, same convention as SizeLimit: the class is "the CELL_SIZE
// thing".
//
// Snapping is RELATIVE: each WM_SIZING adjusts the dragged edge so
// the size delta from the current window rect is a whole number of
// cells. That gives the increment "detent" feel without the tag
// having to know anything about window chrome, the tab strip, or
// padding — and because the renderer reports the cell size in
// physical pixels for the current DPI, per-monitor DPI changes are
// handled by ghostty re-reporting CELL_SIZE.
//
// The subclass installs lazily on the first Apply so windows that
// never receive a CELL_SIZE don't pay the subclass cost. Win32
// auto-removes subclasses when the HWND is destroyed, so no
// explicit teardown is needed. Maximize and fullscreen never enter
// the interactive sizing loop, so no guard is needed there.
class CellSize {
public:
    CellSize() = default;
    CellSize(const CellSize&) = delete;
    CellSize& operator=(const CellSize&) = delete;

    // Update the active cell metrics and install the subclass on
    // first use. Zero in either dimension disables snapping on
    // that axis (defensive — the renderer always reports both).
    void Apply(HWND hwnd, ghostty_action_cell_size_s cell) noexcept;

private:
    static LRESULT CALLBACK SubclassProc(
        HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
        UINT_PTR id, DWORD_PTR ref) noexcept;

    ghostty_action_cell_size_s m_value{};
    bool m_subclassed = false;
};

}  // namespace core::ghostty::actions::tags
