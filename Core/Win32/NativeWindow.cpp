#include "Win32/NativeWindow.h"
#include "Win32/DebugTrace.h"
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace core::win32 {

namespace {
constexpr UINT_PTR kSubclassId = 1;
}

NativeWindow::~NativeWindow()
{
    RemoveSubclass();
}

void NativeWindow::Bind(HWND hwnd) noexcept
{
    if (hwnd == m_hwnd) return;
    RemoveSubclass();
    m_hwnd = hwnd;
    m_fullscreen = false;
    // Rules set before the handle existed take effect now.
    if (m_hasSizeLimit || m_cellSnap.Snaps()) EnsureSubclass();
}

void NativeWindow::SetSizeLimit(ghostty::actions::tags::SizeLimit const& limit) noexcept
{
    m_sizeLimit = limit;
    m_hasSizeLimit = true;
    EnsureSubclass();
}

void NativeWindow::SetCellSnap(ghostty::actions::tags::CellSize const& cells) noexcept
{
    m_cellSnap = cells;
    // Install as soon as metrics are known, even with the gate off:
    // a config reload can turn snapping on without a new CELL_SIZE,
    // and the proc checks the gate per message.
    if (m_cellSnap.Value().width != 0 && m_cellSnap.Value().height != 0) EnsureSubclass();
}

void NativeWindow::EnterFullscreen() noexcept
{
    if (!m_hwnd || m_fullscreen) return;
    m_prevPlacement.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(m_hwnd, &m_prevPlacement);
    m_prevStyle = GetWindowLongPtrW(m_hwnd, GWL_STYLE);

    HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(mon, &mi)) return;

    SetWindowLongPtrW(m_hwnd, GWL_STYLE, m_prevStyle & ~WS_OVERLAPPEDWINDOW);
    SetWindowPos(m_hwnd, HWND_TOP,
                 mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_NOZORDER | SWP_FRAMECHANGED);
    m_fullscreen = true;
}

void NativeWindow::LeaveFullscreen() noexcept
{
    if (!m_hwnd || !m_fullscreen) return;
    SetWindowLongPtrW(m_hwnd, GWL_STYLE, m_prevStyle);
    SetWindowPlacement(m_hwnd, &m_prevPlacement);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    m_fullscreen = false;
}

void NativeWindow::EnsureSubclass() noexcept
{
    if (m_subclassed || !m_hwnd) return;
    if (SetWindowSubclass(m_hwnd, &SubclassProc, kSubclassId,
                          reinterpret_cast<DWORD_PTR>(this))) {
        m_subclassed = true;
    }
}

void NativeWindow::RemoveSubclass() noexcept
{
    if (!m_subclassed) return;
    if (m_hwnd && IsWindow(m_hwnd)) {
        RemoveWindowSubclass(m_hwnd, &SubclassProc, kSubclassId);
    }
    m_subclassed = false;
}

void NativeWindow::OnGetMinMaxInfo(MINMAXINFO& mmi) const noexcept
{
    if (!m_hasSizeLimit) return;
    const auto track = m_sizeLimit.Clamp({
        mmi.ptMinTrackSize.x, mmi.ptMinTrackSize.y,
        mmi.ptMaxTrackSize.x, mmi.ptMaxTrackSize.y,
    });
    mmi.ptMinTrackSize = { track.minWidth, track.minHeight };
    mmi.ptMaxTrackSize = { track.maxWidth, track.maxHeight };
}

bool NativeWindow::OnSizing(WPARAM edge, RECT& drag) const noexcept
{
    if (!m_cellSnap.Snaps()) return false;
    RECT cur{};
    if (!GetWindowRect(m_hwnd, &cur)) return false;
    // Snap the delta from the current rect on whichever edges this
    // drag moves. The current rect was itself produced by the
    // previous snapped WM_SIZING, so the grid stays coherent through
    // the whole sizing loop.
    switch (edge) {
        case WMSZ_LEFT: case WMSZ_TOPLEFT: case WMSZ_BOTTOMLEFT:
            drag.left = m_cellSnap.SnapHorizontal(cur.left, drag.left, -1);
            break;
        default: break;
    }
    switch (edge) {
        case WMSZ_RIGHT: case WMSZ_TOPRIGHT: case WMSZ_BOTTOMRIGHT:
            drag.right = m_cellSnap.SnapHorizontal(cur.right, drag.right, +1);
            break;
        default: break;
    }
    switch (edge) {
        case WMSZ_TOP: case WMSZ_TOPLEFT: case WMSZ_TOPRIGHT:
            drag.top = m_cellSnap.SnapVertical(cur.top, drag.top, -1);
            break;
        default: break;
    }
    switch (edge) {
        case WMSZ_BOTTOM: case WMSZ_BOTTOMLEFT: case WMSZ_BOTTOMRIGHT:
            drag.bottom = m_cellSnap.SnapVertical(cur.bottom, drag.bottom, +1);
            break;
        default: break;
    }
    return true;
}

LRESULT CALLBACK NativeWindow::SubclassProc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR /*id*/, DWORD_PTR ref) noexcept
{
    auto* self = reinterpret_cast<NativeWindow*>(ref);
    if (self) {
        switch (msg) {
            case WM_GETMINMAXINFO:
                self->OnGetMinMaxInfo(*reinterpret_cast<MINMAXINFO*>(lp));
                break;
            case WM_ENTERSIZEMOVE:
                DEBUG_TRACE(L"CellSnap: sizing loop enter, enabled=%d cell=%ux%u\n",
                            self->m_cellSnap.Enabled() ? 1 : 0,
                            self->m_cellSnap.Value().width,
                            self->m_cellSnap.Value().height);
                break;
            case WM_SIZING:
                if (auto* drag = reinterpret_cast<RECT*>(lp);
                    drag && self->OnSizing(wp, *drag)) {
                    return TRUE;  // WM_SIZING contract: TRUE = rect adjusted
                }
                break;
            default: break;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace core::win32
