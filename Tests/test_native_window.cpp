#include "pch.h"
#include "../Core/Win32/NativeWindow.h"
#include <windows.h>

using core::win32::NativeWindow;
using core::ghostty::actions::tags::CellSize;
using core::ghostty::actions::tags::SizeLimit;

namespace {

// Message-only window for driving the subclass proc directly.
// HWND_MESSAGE means it never appears on screen and the OS won't
// route normal input messages to it — but SendMessage delivers
// straight to the subclass chain, which is what we want here.
// STATIC reuses an existing window class so no RegisterClass /
// WindowProc plumbing is needed.
HWND CreateMessageOnlyWindow() {
    return CreateWindowExW(
        0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// Top-level WS_OVERLAPPEDWINDOW window without WS_VISIBLE so
// fullscreen has a real style to snapshot / strip / restore but
// nothing flashes on screen. MonitorFromWindow returns the nearest
// monitor even for hidden windows.
HWND CreateHiddenTopLevel() {
    return CreateWindowExW(
        0, L"STATIC", L"", WS_OVERLAPPEDWINDOW,
        0, 0, 400, 300,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// Send a WM_GETMINMAXINFO with the given defaults and return the
// MINMAXINFO the subclass left behind.
MINMAXINFO ProbeMinMax(HWND hwnd,
                       LONG defMinX, LONG defMinY,
                       LONG defMaxX, LONG defMaxY) {
    MINMAXINFO mmi{};
    mmi.ptMinTrackSize = { defMinX, defMinY };
    mmi.ptMaxTrackSize = { defMaxX, defMaxY };
    SendMessageW(hwnd, WM_GETMINMAXINFO, 0,
                 reinterpret_cast<LPARAM>(&mmi));
    return mmi;
}

SizeLimit MinWidth(unsigned w) {
    ghostty_action_size_limit_s limit{};
    limit.min_width = w;
    SizeLimit s;
    s.Apply(limit);
    return s;
}

CellSize Cells(unsigned w, unsigned h, bool enabled) {
    ghostty_action_cell_size_s c{};
    c.width = w;
    c.height = h;
    CellSize cs;
    cs.Apply(c, enabled);
    return cs;
}

}  // namespace

// ----- null safety -----

TEST(NativeWindowTest, RulesWithoutAHandleAreSafe) {
    NativeWindow w;
    w.SetSizeLimit(MinWidth(100));
    w.SetCellSnap(Cells(12, 31, true));
    w.EnterFullscreen();
    w.LeaveFullscreen();
    EXPECT_FALSE(w.InFullscreen());
}

// ----- size limit through WM_GETMINMAXINFO -----

TEST(NativeWindowTest, SizeLimitReachesMinMaxInfo) {
    HWND hwnd = CreateMessageOnlyWindow();
    ASSERT_NE(hwnd, nullptr);

    NativeWindow w;
    w.Bind(hwnd);
    w.SetSizeLimit(MinWidth(200));

    auto mmi = ProbeMinMax(hwnd, 0, 0, 9999, 9999);
    EXPECT_EQ(mmi.ptMinTrackSize.x, 200);
    EXPECT_EQ(mmi.ptMaxTrackSize.x, 9999);   // untouched

    DestroyWindow(hwnd);
}

TEST(NativeWindowTest, RuleSetBeforeBindTakesEffectOnBind) {
    // The tear-out order: the host learns its rules (adopt) before
    // its first Activated hands it the HWND.
    HWND hwnd = CreateMessageOnlyWindow();
    ASSERT_NE(hwnd, nullptr);

    NativeWindow w;
    w.SetSizeLimit(MinWidth(300));
    w.Bind(hwnd);

    EXPECT_EQ(ProbeMinMax(hwnd, 0, 0, 9999, 9999).ptMinTrackSize.x, 300);

    DestroyWindow(hwnd);
}

TEST(NativeWindowTest, SecondSetUpdatesWithoutReinstall) {
    HWND hwnd = CreateMessageOnlyWindow();
    ASSERT_NE(hwnd, nullptr);

    NativeWindow w;
    w.Bind(hwnd);
    w.SetSizeLimit(MinWidth(100));
    // Would stack a duplicate subclass entry if the install were
    // repeated, which we'd notice as a doubled clamp; the new value
    // simply takes over.
    w.SetSizeLimit(MinWidth(300));

    EXPECT_EQ(ProbeMinMax(hwnd, 0, 0, 9999, 9999).ptMinTrackSize.x, 300);

    DestroyWindow(hwnd);
}

TEST(NativeWindowTest, IndependentWindowsDontInterfere) {
    HWND hwnd1 = CreateMessageOnlyWindow();
    HWND hwnd2 = CreateMessageOnlyWindow();
    ASSERT_NE(hwnd1, nullptr);
    ASSERT_NE(hwnd2, nullptr);

    NativeWindow a;
    NativeWindow b;
    a.Bind(hwnd1);
    b.Bind(hwnd2);
    a.SetSizeLimit(MinWidth(100));
    b.SetSizeLimit(MinWidth(500));

    EXPECT_EQ(ProbeMinMax(hwnd1, 0, 0, 9999, 9999).ptMinTrackSize.x, 100);
    EXPECT_EQ(ProbeMinMax(hwnd2, 0, 0, 9999, 9999).ptMinTrackSize.x, 500);

    DestroyWindow(hwnd1);
    DestroyWindow(hwnd2);
}

TEST(NativeWindowTest, DestructorLeavesTheWindowUnsubclassed) {
    HWND hwnd = CreateMessageOnlyWindow();
    ASSERT_NE(hwnd, nullptr);
    {
        NativeWindow w;
        w.Bind(hwnd);
        w.SetSizeLimit(MinWidth(200));
        EXPECT_EQ(ProbeMinMax(hwnd, 0, 0, 9999, 9999).ptMinTrackSize.x, 200);
    }
    // The object is gone; the window must not call into it any more.
    EXPECT_EQ(ProbeMinMax(hwnd, 0, 0, 9999, 9999).ptMinTrackSize.x, 0);
    DestroyWindow(hwnd);
}

// ----- cell snap through WM_SIZING -----

TEST(NativeWindowTest, CellSnapAdjustsTheDraggedEdge) {
    HWND hwnd = CreateMessageOnlyWindow();
    ASSERT_NE(hwnd, nullptr);
    RECT cur{};
    ASSERT_TRUE(GetWindowRect(hwnd, &cur));

    NativeWindow w;
    w.Bind(hwnd);
    w.SetCellSnap(Cells(12, 31, /*enabled=*/true));

    // Drag the right edge 25px out: 25/12 → 2 cells → +24.
    RECT drag = cur;
    drag.right = cur.right + 25;
    const LRESULT handled = SendMessageW(hwnd, WM_SIZING, WMSZ_RIGHT,
                                         reinterpret_cast<LPARAM>(&drag));
    EXPECT_EQ(handled, TRUE);
    EXPECT_EQ(drag.right, cur.right + 24);
    EXPECT_EQ(drag.left, cur.left);   // untouched edge

    DestroyWindow(hwnd);
}

TEST(NativeWindowTest, CellSnapIsOffWhileTheGateIsOff) {
    HWND hwnd = CreateMessageOnlyWindow();
    ASSERT_NE(hwnd, nullptr);
    RECT cur{};
    ASSERT_TRUE(GetWindowRect(hwnd, &cur));

    NativeWindow w;
    w.Bind(hwnd);
    w.SetCellSnap(Cells(12, 31, /*enabled=*/false));

    RECT drag = cur;
    drag.right = cur.right + 25;
    SendMessageW(hwnd, WM_SIZING, WMSZ_RIGHT, reinterpret_cast<LPARAM>(&drag));
    EXPECT_EQ(drag.right, cur.right + 25);   // free resize

    DestroyWindow(hwnd);
}

// ----- fullscreen: style strip + placement round-trip -----

TEST(NativeWindowTest, EnterFullscreenStripsOverlappedWindowStyle) {
    HWND hwnd = CreateHiddenTopLevel();
    ASSERT_NE(hwnd, nullptr);
    ASSERT_TRUE(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW);

    NativeWindow w;
    w.Bind(hwnd);
    w.EnterFullscreen();

    EXPECT_TRUE(w.InFullscreen());
    EXPECT_FALSE(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW);

    DestroyWindow(hwnd);
}

TEST(NativeWindowTest, LeaveFullscreenRestoresStyle) {
    HWND hwnd = CreateHiddenTopLevel();
    ASSERT_NE(hwnd, nullptr);
    const LONG_PTR before = GetWindowLongPtrW(hwnd, GWL_STYLE);

    NativeWindow w;
    w.Bind(hwnd);
    w.EnterFullscreen();
    w.LeaveFullscreen();

    // WS_VISIBLE is masked out: SetWindowPlacement on exit honours
    // showCmd=SW_SHOWNORMAL (the default for a freshly-created
    // window), which flips the bit on; in production the window was
    // already visible going in, so this is a fixture artefact.
    EXPECT_FALSE(w.InFullscreen());
    EXPECT_EQ(GetWindowLongPtrW(hwnd, GWL_STYLE) & ~WS_VISIBLE,
              before & ~WS_VISIBLE);

    DestroyWindow(hwnd);
}

TEST(NativeWindowTest, EnterFullscreenSpansTheMonitor) {
    HWND hwnd = CreateHiddenTopLevel();
    ASSERT_NE(hwnd, nullptr);
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    ASSERT_TRUE(GetMonitorInfoW(mon, &mi));

    NativeWindow w;
    w.Bind(hwnd);
    w.EnterFullscreen();

    // The full monitor rect, not the work area — the title bar and
    // borders are stripped too.
    RECT actual{};
    ASSERT_TRUE(GetWindowRect(hwnd, &actual));
    EXPECT_EQ(actual.left,   mi.rcMonitor.left);
    EXPECT_EQ(actual.top,    mi.rcMonitor.top);
    EXPECT_EQ(actual.right,  mi.rcMonitor.right);
    EXPECT_EQ(actual.bottom, mi.rcMonitor.bottom);

    DestroyWindow(hwnd);
}

TEST(NativeWindowTest, LeaveFullscreenRestoresPlacement) {
    HWND hwnd = CreateHiddenTopLevel();
    ASSERT_NE(hwnd, nullptr);
    WINDOWPLACEMENT before{ sizeof(WINDOWPLACEMENT) };
    ASSERT_TRUE(GetWindowPlacement(hwnd, &before));

    NativeWindow w;
    w.Bind(hwnd);
    w.EnterFullscreen();
    w.LeaveFullscreen();

    WINDOWPLACEMENT after{ sizeof(WINDOWPLACEMENT) };
    ASSERT_TRUE(GetWindowPlacement(hwnd, &after));
    // rcNormalPosition round-trips through the snapshot — the field
    // that captures a maximised window correctly (the reason for
    // WINDOWPLACEMENT rather than a bare RECT).
    EXPECT_EQ(after.rcNormalPosition.left,   before.rcNormalPosition.left);
    EXPECT_EQ(after.rcNormalPosition.top,    before.rcNormalPosition.top);
    EXPECT_EQ(after.rcNormalPosition.right,  before.rcNormalPosition.right);
    EXPECT_EQ(after.rcNormalPosition.bottom, before.rcNormalPosition.bottom);

    DestroyWindow(hwnd);
}

TEST(NativeWindowTest, RepeatedEnterOrLeaveIsANoOp) {
    HWND hwnd = CreateHiddenTopLevel();
    ASSERT_NE(hwnd, nullptr);
    const LONG_PTR before = GetWindowLongPtrW(hwnd, GWL_STYLE);

    NativeWindow w;
    w.Bind(hwnd);
    w.LeaveFullscreen();          // not in: nothing to restore
    w.EnterFullscreen();
    w.EnterFullscreen();          // already in: must not re-snapshot the stripped style
    w.LeaveFullscreen();

    EXPECT_EQ(GetWindowLongPtrW(hwnd, GWL_STYLE) & ~WS_VISIBLE,
              before & ~WS_VISIBLE);

    DestroyWindow(hwnd);
}
