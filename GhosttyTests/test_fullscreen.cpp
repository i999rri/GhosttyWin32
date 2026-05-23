#include "pch.h"
#include "../GhosttyWin32App/Fullscreen.h"

using winrt::GhosttyWin32::implementation::Fullscreen;

// Like SizeLimit, the real behaviour (placement snapshot, style
// swap, MonitorFromWindow span) lives behind Win32 calls that
// can't be driven without a window. The unit-testable surface is
// the null-HWND contract.

TEST(FullscreenTest, ToggleWithNullHwndIsSafeNoOp) {
    Fullscreen f;
    f.Toggle(nullptr);
    // No crash; internal state still says "not active", so a
    // subsequent Toggle with a real HWND would correctly take the
    // "enter fullscreen" branch — verifying that needs a window.
}

TEST(FullscreenTest, RepeatedToggleWithNullHwndStaysSafe) {
    Fullscreen f;
    f.Toggle(nullptr);
    f.Toggle(nullptr);
    f.Toggle(nullptr);
}
