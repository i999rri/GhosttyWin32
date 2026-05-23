#include "Fullscreen.h"

namespace winrt::GhosttyWin32::implementation::core::ghostty::actions::tags {

void Fullscreen::Toggle(HWND hwnd) noexcept
{
    if (!hwnd) return;
    if (!m_active) {
        m_prevPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(hwnd, &m_prevPlacement);
        m_prevStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);

        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(mon, &mi)) return;

        SetWindowLongPtrW(hwnd, GWL_STYLE,
                          m_prevStyle & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOZORDER | SWP_FRAMECHANGED);
        m_active = true;
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, m_prevStyle);
        SetWindowPlacement(hwnd, &m_prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        m_active = false;
    }
}

}  // namespace winrt::GhosttyWin32::implementation::core::ghostty::actions::tags
