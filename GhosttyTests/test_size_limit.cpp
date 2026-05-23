#include "pch.h"
#include "../GhosttyWin32App/SizeLimit.h"

using winrt::GhosttyWin32::implementation::SizeLimit;

// The interesting parts of SizeLimit live in the WM_GETMINMAXINFO
// subclass proc, which needs a real HWND to exercise — out of
// scope for a unit test. What we can pin down here is the null-
// HWND contract: callers (GhosttyActions / IMainWindowView::
// ApplySizeLimit) might run before the host window is created,
// and the value-object must not crash or install a subclass
// against a null handle.

TEST(SizeLimitTest, ApplyWithNullHwndIsSafeNoOp) {
    SizeLimit s;
    ghostty_action_size_limit_s limit{};
    limit.min_width = 100;
    limit.min_height = 50;
    limit.max_width = 800;
    limit.max_height = 600;
    s.Apply(nullptr, limit);
    // No crash, no Win32 side effects. A subsequent Apply with a
    // real HWND would still be the first install, but verifying
    // that requires a real window — left to integration tests.
}

TEST(SizeLimitTest, RepeatedApplyWithNullHwndStaysSafe) {
    SizeLimit s;
    ghostty_action_size_limit_s limit{};
    limit.min_width = 200;
    // Calling twice exercises the "already-subclassed?" branch on
    // both passes; with a null HWND, both should bail before the
    // Win32 call.
    s.Apply(nullptr, limit);
    s.Apply(nullptr, limit);
}
