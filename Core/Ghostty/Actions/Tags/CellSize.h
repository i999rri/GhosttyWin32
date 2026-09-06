#pragma once

#include "ghostty.h"
#include <cmath>

namespace core::ghostty::actions::tags {

// CELL_SIZE action value (the cell's pixel dimensions), the
// `window-step-resize` gate, and the one decision made from them:
// where a dragged window edge should land so the window grows and
// shrinks in whole cells — the Win32 counterpart of macOS's
// contentResizeIncrements (#155).
//
// Snapping is RELATIVE: each step adjusts the dragged edge so the
// size delta from the current window rect is a whole number of
// cells. That gives the increment "detent" feel without knowing
// anything about window chrome, the tab strip, or padding — and
// because the renderer reports the cell size in physical pixels for
// the current DPI, per-monitor DPI changes are handled by ghostty
// re-reporting CELL_SIZE.
//
// Pure value object. Applying the snap to a window is
// win32::NativeWindow's job (WM_SIZING); this class never sees an
// HWND, so the rounding is unit-testable on its own.
class CellSize {
public:
    CellSize() = default;

    // Take a CELL_SIZE report together with the current gate
    // (Config::WindowStepResize(), upstream default: false). A report
    // with a zero dimension is ignored — nothing has been measured
    // yet, and the previous metrics stay.
    void Apply(ghostty_action_cell_size_s cell, bool stepResize) noexcept {
        if (cell.width == 0 || cell.height == 0) return;
        m_value = cell;
        m_enabled = stepResize;
    }

    // Re-read of the gate alone, for a config reload: CELL_SIZE only
    // re-fires on metric changes, so a reload that flips
    // `window-step-resize` by itself must reach the gate this way.
    void SetEnabled(bool stepResize) noexcept { m_enabled = stepResize; }
    bool Enabled() const noexcept { return m_enabled; }

    ghostty_action_cell_size_s const& Value() const noexcept { return m_value; }

    // Whether a drag should snap at all: gate on and metrics known.
    bool Snaps() const noexcept {
        return m_enabled && m_value.width != 0 && m_value.height != 0;
    }

    // Where the dragged edge lands. `anchor` is the edge's position
    // in the current window rect, `moving` where the drag has put it,
    // `direction` +1 when the edge grows away from the anchor toward
    // positive coordinates (right / bottom), -1 otherwise (left /
    // top). Uses the width for horizontal edges, the height for
    // vertical ones.
    long SnapHorizontal(long anchor, long moving, int direction) const noexcept {
        return Snap(anchor, moving, static_cast<long>(m_value.width), direction);
    }
    long SnapVertical(long anchor, long moving, int direction) const noexcept {
        return Snap(anchor, moving, static_cast<long>(m_value.height), direction);
    }

    // The rounding itself, for one axis. The drag delta is
    // legitimately negative when the user is shrinking the window, so
    // it must not be clamped (that made resize grow-only) and the
    // rounding must be symmetric around zero — integer division
    // truncates toward zero and would bias the negative side, hence
    // lround. A non-positive step leaves the edge where the drag put
    // it.
    static long Snap(long anchor, long moving, long step, int direction) noexcept {
        if (step <= 0) return moving;
        const long span = (moving - anchor) * direction;
        const long snapped = static_cast<long>(
            std::lround(static_cast<double>(span) / step) * step);
        return anchor + snapped * direction;
    }

private:
    ghostty_action_cell_size_s m_value{};
    bool m_enabled = false;
};

}  // namespace core::ghostty::actions::tags
