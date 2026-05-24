#include "SizeLimit.h"
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace winrt::GhosttyWin32::implementation {

void SizeLimit::Apply(HWND hwnd, ghostty_action_size_limit_s limit) noexcept
{
    m_value = limit;
    if (m_subclassed || !hwnd) return;
    if (SetWindowSubclass(hwnd, &SubclassProc, 1,
                          reinterpret_cast<DWORD_PTR>(this))) {
        m_subclassed = true;
    }
}

LRESULT CALLBACK SizeLimit::SubclassProc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR /*id*/, DWORD_PTR ref) noexcept
{
    if (msg == WM_GETMINMAXINFO) {
        auto* self = reinterpret_cast<SizeLimit*>(ref);
        if (self) {
            auto& v = self->m_value;
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            // Zero in any field means "no limit" — leave the
            // default. Only the populated fields override.
            if (v.min_width)  mmi->ptMinTrackSize.x = static_cast<LONG>(v.min_width);
            if (v.min_height) mmi->ptMinTrackSize.y = static_cast<LONG>(v.min_height);
            if (v.max_width)  mmi->ptMaxTrackSize.x = static_cast<LONG>(v.max_width);
            if (v.max_height) mmi->ptMaxTrackSize.y = static_cast<LONG>(v.max_height);
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace winrt::GhosttyWin32::implementation
