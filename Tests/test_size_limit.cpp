#include "pch.h"
#include "../App/SizeLimit.h"
#include <windows.h>

using winrt::GhosttyWin32::implementation::SizeLimit;

namespace {

// Message-only window for testing the subclass proc directly.
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

// Send a WM_GETMINMAXINFO with the given defaults and return the
// MINMAXINFO the subclass left behind. The subclass overrides
// only the fields ghostty populated; everything else flows
// through untouched.
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

}  // namespace

// ----- null safety -----

TEST(SizeLimitTest, ApplyWithNullHwndIsSafeNoOp) {
    SizeLimit s;
    ghostty_action_size_limit_s limit{};
    limit.min_width = 100;
    s.Apply(nullptr, limit);
    // No crash, no subclass installed — verified indirectly: a
    // later Apply with a real HWND would install fresh.
}

// ----- subclass proc behaviour -----

TEST(SizeLimitTest, ClampsMinTrackSize) {
    HWND hwnd = CreateMessageOnlyWindow();
    ASSERT_NE(hwnd, nullptr);

    SizeLimit s;
    ghostty_action_size_limit_s limit{};
    limit.min_width = 200;
    limit.min_height = 150;
    s.Apply(hwnd, limit);

    auto mmi = ProbeMinMax(hwnd, /*defMin*/ 0, 0, /*defMax*/ 9999, 9999);
    EXPECT_EQ(mmi.ptMinTrackSize.x, 200);
    EXPECT_EQ(mmi.ptMinTrackSize.y, 150);
    // Max wasn't populated → defaults preserved.
    EXPECT_EQ(mmi.ptMaxTrackSize.x, 9999);
    EXPECT_EQ(mmi.ptMaxTrackSize.y, 9999);

    DestroyWindow(hwnd);
}

TEST(SizeLimitTest, ClampsMaxTrackSize) {
    HWND hwnd = CreateMessageOnlyWindow();
    ASSERT_NE(hwnd, nullptr);

    SizeLimit s;
    ghostty_action_size_limit_s limit{};
    limit.max_width = 800;
    limit.max_height = 600;
    s.Apply(hwnd, limit);

    auto mmi = ProbeMinMax(hwnd, 0, 0, 9999, 9999);
    EXPECT_EQ(mmi.ptMaxTrackSize.x, 800);
    EXPECT_EQ(mmi.ptMaxTrackSize.y, 600);
    // Min wasn't populated → defaults preserved.
    EXPECT_EQ(mmi.ptMinTrackSize.x, 0);
    EXPECT_EQ(mmi.ptMinTrackSize.y, 0);

    DestroyWindow(hwnd);
}

TEST(SizeLimitTest, ZeroFieldsLeaveDefaultsUntouched) {
    HWND hwnd = CreateMessageOnlyWindow();
    ASSERT_NE(hwnd, nullptr);

    SizeLimit s;
    ghostty_action_size_limit_s limit{};  // all zero — "no override"
    s.Apply(hwnd, limit);

    auto mmi = ProbeMinMax(hwnd, /*defMin*/ 100, 50, /*defMax*/ 1000, 500);
    // None of the fields were populated, so the OS-supplied
    // defaults must pass through untouched.
    EXPECT_EQ(mmi.ptMinTrackSize.x, 100);
    EXPECT_EQ(mmi.ptMinTrackSize.y, 50);
    EXPECT_EQ(mmi.ptMaxTrackSize.x, 1000);
    EXPECT_EQ(mmi.ptMaxTrackSize.y, 500);

    DestroyWindow(hwnd);
}

TEST(SizeLimitTest, SecondApplyUpdatesValueWithoutReinstall) {
    HWND hwnd = CreateMessageOnlyWindow();
    ASSERT_NE(hwnd, nullptr);

    SizeLimit s;
    ghostty_action_size_limit_s first{};
    first.min_width = 100;
    s.Apply(hwnd, first);

    // Second Apply exercises the "already-subclassed" branch
    // (Win32 would otherwise stack a duplicate subclass entry,
    // which we'd notice as a doubled clamp). New value takes
    // over.
    ghostty_action_size_limit_s second{};
    second.min_width = 300;
    s.Apply(hwnd, second);

    auto mmi = ProbeMinMax(hwnd, 0, 0, 9999, 9999);
    EXPECT_EQ(mmi.ptMinTrackSize.x, 300);

    DestroyWindow(hwnd);
}

TEST(SizeLimitTest, IndependentInstancesDontInterfere) {
    HWND hwnd1 = CreateMessageOnlyWindow();
    HWND hwnd2 = CreateMessageOnlyWindow();
    ASSERT_NE(hwnd1, nullptr);
    ASSERT_NE(hwnd2, nullptr);

    SizeLimit a;
    SizeLimit b;
    ghostty_action_size_limit_s la{}; la.min_width = 100;
    ghostty_action_size_limit_s lb{}; lb.min_width = 500;
    a.Apply(hwnd1, la);
    b.Apply(hwnd2, lb);

    auto mmi1 = ProbeMinMax(hwnd1, 0, 0, 9999, 9999);
    auto mmi2 = ProbeMinMax(hwnd2, 0, 0, 9999, 9999);
    EXPECT_EQ(mmi1.ptMinTrackSize.x, 100);
    EXPECT_EQ(mmi2.ptMinTrackSize.x, 500);

    DestroyWindow(hwnd1);
    DestroyWindow(hwnd2);
}
