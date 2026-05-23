#include "pch.h"
#include "SizeLimiter.h"
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace winrt::GhosttyWin32::implementation {

void SizeLimiter::Apply(HWND hwnd, ghostty_action_size_limit_s limit) noexcept
{
    m_limit = limit;
    if (m_subclassed || !hwnd) return;
    if (SetWindowSubclass(hwnd, &SubclassProc, 1,
                          reinterpret_cast<DWORD_PTR>(this))) {
        m_subclassed = true;
    }
}

LRESULT CALLBACK SizeLimiter::SubclassProc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR /*id*/, DWORD_PTR ref) noexcept
{
    if (msg == WM_GETMINMAXINFO) {
        auto* self = reinterpret_cast<SizeLimiter*>(ref);
        if (self) {
            auto& sl = self->m_limit;
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            // Zero in any field means "no limit" — leave the
            // default. Only the populated fields override.
            if (sl.min_width)  mmi->ptMinTrackSize.x = static_cast<LONG>(sl.min_width);
            if (sl.min_height) mmi->ptMinTrackSize.y = static_cast<LONG>(sl.min_height);
            if (sl.max_width)  mmi->ptMaxTrackSize.x = static_cast<LONG>(sl.max_width);
            if (sl.max_height) mmi->ptMaxTrackSize.y = static_cast<LONG>(sl.max_height);
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace winrt::GhosttyWin32::implementation
