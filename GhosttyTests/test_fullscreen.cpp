#include "pch.h"
#include "../GhosttyWin32App/Fullscreen.h"
#include <windows.h>

using winrt::GhosttyWin32::implementation::Fullscreen;

namespace {

// Top-level WS_OVERLAPPEDWINDOW window without WS_VISIBLE so
// Fullscreen has a real style to snapshot / strip / restore but
// nothing flashes on screen. MonitorFromWindow returns the
// nearest monitor even for hidden windows, which is what
// Fullscreen::Toggle relies on.
HWND CreateHiddenTopLevel() {
    return CreateWindowExW(
        0, L"STATIC", L"", WS_OVERLAPPEDWINDOW,
        0, 0, 400, 300,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

}  // namespace

// ----- null safety -----

TEST(FullscreenTest, ToggleWithNullHwndIsSafeNoOp) {
    Fullscreen f;
    f.Toggle(nullptr);
    f.Toggle(nullptr);
}

// ----- style strip + restore round-trip -----

TEST(FullscreenTest, ToggleStripsOverlappedwindowStyle) {
    HWND hwnd = CreateHiddenTopLevel();
    ASSERT_NE(hwnd, nullptr);
    ASSERT_TRUE(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW);

    Fullscreen f;
    f.Toggle(hwnd);

    // Entering fullscreen drops WS_OVERLAPPEDWINDOW (caption +
    // borders + sysmenu bits) so the window can span the monitor
    // without chrome.
    EXPECT_FALSE(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW);

    DestroyWindow(hwnd);
}

TEST(FullscreenTest, ToggleTwiceRestoresStyle) {
    HWND hwnd = CreateHiddenTopLevel();
    ASSERT_NE(hwnd, nullptr);
    const LONG_PTR before = GetWindowLongPtrW(hwnd, GWL_STYLE);

    Fullscreen f;
    f.Toggle(hwnd);
    f.Toggle(hwnd);

    // Leaving fullscreen must put back the same style we
    // snapshotted on entry — that's the whole reason
    // FullscreenController stores m_prevStyle.
    EXPECT_EQ(GetWindowLongPtrW(hwnd, GWL_STYLE), before);

    DestroyWindow(hwnd);
}

TEST(FullscreenTest, ToggleSpansMonitor) {
    HWND hwnd = CreateHiddenTopLevel();
    ASSERT_NE(hwnd, nullptr);

    // Capture the monitor the window will use so we have an
    // expected rect to compare against.
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    ASSERT_TRUE(GetMonitorInfoW(mon, &mi));

    Fullscreen f;
    f.Toggle(hwnd);

    // After entering fullscreen the window rect should match
    // the monitor's full rect (not the work area — the title bar
    // and borders are also stripped).
    RECT actual{};
    ASSERT_TRUE(GetWindowRect(hwnd, &actual));
    EXPECT_EQ(actual.left,   mi.rcMonitor.left);
    EXPECT_EQ(actual.top,    mi.rcMonitor.top);
    EXPECT_EQ(actual.right,  mi.rcMonitor.right);
    EXPECT_EQ(actual.bottom, mi.rcMonitor.bottom);

    DestroyWindow(hwnd);
}

TEST(FullscreenTest, ToggleTwiceRestoresPlacement) {
    HWND hwnd = CreateHiddenTopLevel();
    ASSERT_NE(hwnd, nullptr);

    WINDOWPLACEMENT before{ sizeof(WINDOWPLACEMENT) };
    ASSERT_TRUE(GetWindowPlacement(hwnd, &before));

    Fullscreen f;
    f.Toggle(hwnd);
    f.Toggle(hwnd);

    WINDOWPLACEMENT after{ sizeof(WINDOWPLACEMENT) };
    ASSERT_TRUE(GetWindowPlacement(hwnd, &after));
    // rcNormalPosition round-trips through the snapshot — this is
    // the field that captures a maximised window correctly (the
    // reason we use WINDOWPLACEMENT rather than a bare RECT).
    EXPECT_EQ(after.rcNormalPosition.left,   before.rcNormalPosition.left);
    EXPECT_EQ(after.rcNormalPosition.top,    before.rcNormalPosition.top);
    EXPECT_EQ(after.rcNormalPosition.right,  before.rcNormalPosition.right);
    EXPECT_EQ(after.rcNormalPosition.bottom, before.rcNormalPosition.bottom);

    DestroyWindow(hwnd);
}
