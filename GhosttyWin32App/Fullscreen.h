#pragma once

#include <windows.h>

namespace winrt::GhosttyWin32::implementation {

// TOGGLE_FULLSCREEN action state. Named after the ghostty action
// tag so the relationship with action_cb is obvious; the class
// is "the fullscreen thing", not "something that controls
// fullscreen".
//
// Holds the pre-fullscreen window state (placement + style) so
// leaving fullscreen restores the exact window the user had —
// including a maximised state (RECT alone would lose that,
// WINDOWPLACEMENT round-trips it).
//
// Caveat carried over from the previous inline implementation:
// the custom title bar lives in the XAML content tree, so it
// stays visible at the top of the surface in fullscreen. Hiding
// it is a follow-up; the window itself does fill the monitor
// correctly.
class Fullscreen {
public:
    Fullscreen() = default;
    Fullscreen(const Fullscreen&) = delete;
    Fullscreen& operator=(const Fullscreen&) = delete;

    // Toggle fullscreen on `hwnd`. On entry, snapshot placement +
    // style and span the monitor; on exit, restore. No-op if the
    // HWND is null.
    void Toggle(HWND hwnd) noexcept;

private:
    bool m_active = false;
    WINDOWPLACEMENT m_prevPlacement{};
    LONG_PTR m_prevStyle = 0;
};

}  // namespace winrt::GhosttyWin32::implementation
