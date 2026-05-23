#pragma once

#include "ghostty.h"
#include <windows.h>

namespace winrt::GhosttyWin32::implementation {

// Encapsulates the WM_GETMINMAXINFO subclass that enforces the
// ghostty SIZE_LIMIT constraint on the host window. State + the
// subclass proc + the install policy all live here; callers
// (MainWindow / IMainWindowView::ApplySizeLimit) just hand over
// the latest limit struct and the HWND.
//
// The subclass installs lazily on the first Apply so windows that
// never receive a SIZE_LIMIT don't pay the subclass cost. Win32
// auto-removes subclasses when the HWND is destroyed, so we don't
// have a matching teardown; the destructor just lets the state
// go away.
class SizeLimiter {
public:
    SizeLimiter() = default;
    SizeLimiter(const SizeLimiter&) = delete;
    SizeLimiter& operator=(const SizeLimiter&) = delete;

    // Update the active limit and install the subclass on first
    // use. Zero in any limit field means "no constraint on this
    // axis" — only populated fields override the default min/max.
    void Apply(HWND hwnd, ghostty_action_size_limit_s limit) noexcept;

private:
    static LRESULT CALLBACK SubclassProc(
        HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
        UINT_PTR id, DWORD_PTR ref) noexcept;

    ghostty_action_size_limit_s m_limit{};
    bool m_subclassed = false;
};

}  // namespace winrt::GhosttyWin32::implementation
