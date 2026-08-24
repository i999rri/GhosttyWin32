#include "CellSize.h"
#include <cmath>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace core::ghostty::actions::tags {

namespace {

// Snap `moving` so that (moving - anchor) is a whole multiple of
// `step`, rounding to the nearest multiple. `direction` is +1 when
// the moving edge grows away from the anchor toward positive
// coordinates (right/bottom edges), -1 otherwise (left/top).
//
// `span` is the DRAG DELTA, not a size: it is legitimately
// negative when the user is shrinking the window, so it must not
// be clamped (clamping made resize grow-only) and the rounding
// must be symmetric around zero — integer division truncates
// toward zero and would bias the negative side, hence lround.
LONG SnapEdge(LONG anchor, LONG moving, LONG step, int direction) noexcept
{
    if (step <= 0) return moving;
    LONG span = (moving - anchor) * direction;
    LONG snapped = static_cast<LONG>(
        std::lround(static_cast<double>(span) / step) * step);
    return anchor + snapped * direction;
}

}  // namespace

void CellSize::Apply(HWND hwnd, ghostty_action_cell_size_s cell) noexcept
{
    m_value = cell;
    if (m_subclassed || !hwnd) return;
    if (SetWindowSubclass(hwnd, &SubclassProc, 1,
                          reinterpret_cast<DWORD_PTR>(this))) {
        m_subclassed = true;
    }
}

LRESULT CALLBACK CellSize::SubclassProc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR /*id*/, DWORD_PTR ref) noexcept
{
    if (msg == WM_SIZING) {
        auto* self = reinterpret_cast<CellSize*>(ref);
        auto* drag = reinterpret_cast<RECT*>(lp);
        RECT cur{};
        if (self && self->m_enabled && drag && GetWindowRect(hwnd, &cur)) {
            const LONG cw = static_cast<LONG>(self->m_value.width);
            const LONG ch = static_cast<LONG>(self->m_value.height);
            // Snap the delta from the current rect on whichever
            // edges this drag moves. The current rect was itself
            // produced by the previous snapped WM_SIZING, so the
            // grid stays coherent through the whole sizing loop.
            switch (wp) {
                case WMSZ_LEFT:
                case WMSZ_TOPLEFT:
                case WMSZ_BOTTOMLEFT:
                    drag->left = SnapEdge(cur.left, drag->left, cw, -1);
                    break;
                default: break;
            }
            switch (wp) {
                case WMSZ_RIGHT:
                case WMSZ_TOPRIGHT:
                case WMSZ_BOTTOMRIGHT:
                    drag->right = SnapEdge(cur.right, drag->right, cw, +1);
                    break;
                default: break;
            }
            switch (wp) {
                case WMSZ_TOP:
                case WMSZ_TOPLEFT:
                case WMSZ_TOPRIGHT:
                    drag->top = SnapEdge(cur.top, drag->top, ch, -1);
                    break;
                default: break;
            }
            switch (wp) {
                case WMSZ_BOTTOM:
                case WMSZ_BOTTOMLEFT:
                case WMSZ_BOTTOMRIGHT:
                    drag->bottom = SnapEdge(cur.bottom, drag->bottom, ch, +1);
                    break;
                default: break;
            }
            return TRUE;  // WM_SIZING contract: TRUE = rect adjusted
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace core::ghostty::actions::tags
