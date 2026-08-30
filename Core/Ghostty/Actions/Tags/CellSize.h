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

    // Take a CELL_SIZE report: record the metrics, set the gate
    // from `window-step-resize` (Config::WindowStepResize(),
    // upstream default: false) and install the subclass on first
    // use. A report with a zero dimension is ignored — nothing has
    // been measured yet, and the previous metrics stay. A null
    // hwnd still records the metrics; call Attach once the HWND
    // exists to complete the install.
    void Apply(HWND hwnd, ghostty_action_cell_size_s cell,
               bool stepResize) noexcept;

    // Late subclass install for the adopt-before-activation order:
    // a fresh tear-out host adopts its tab (and arms the metrics)
    // BEFORE first activation assigns the HWND, so the owner calls
    // this when the HWND finally exists. No-op when already
    // installed or when no metrics have been recorded yet (windows
    // that never saw a CELL_SIZE keep paying no subclass cost).
    void Attach(HWND hwnd) noexcept;

    // Re-read of the gate alone, for a config reload: CELL_SIZE
    // only re-fires on metric changes, so a reload that flips
    // `window-step-resize` by itself must reach the gate this way.
    void SetEnabled(bool stepResize) noexcept { m_enabled = stepResize; }
    bool Enabled() const noexcept { return m_enabled; }

private:
    static LRESULT CALLBACK SubclassProc(
        HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
        UINT_PTR id, DWORD_PTR ref) noexcept;

    ghostty_action_cell_size_s m_value{};
    bool m_enabled = false;
    bool m_subclassed = false;
};

}  // namespace core::ghostty::actions::tags
