#include "pch.h"
#include "../Core/Ghostty/Actions/Tags/SizeLimit.h"

using core::ghostty::actions::tags::SizeLimit;

namespace {

SizeLimit::Track Proposed(long minW, long minH, long maxW, long maxH) {
    return { minW, minH, maxW, maxH };
}

}  // namespace

// The rule: populated fields override what the OS proposes, zero
// fields leave it alone. The window side (NativeWindow) feeds
// WM_GETMINMAXINFO through Clamp and writes the result back.

TEST(SizeLimitTest, DefaultLeavesEverythingAsProposed) {
    SizeLimit s;
    auto t = s.Clamp(Proposed(100, 50, 1000, 500));
    EXPECT_EQ(t.minWidth, 100);
    EXPECT_EQ(t.minHeight, 50);
    EXPECT_EQ(t.maxWidth, 1000);
    EXPECT_EQ(t.maxHeight, 500);
}

TEST(SizeLimitTest, ClampsMinTrackSize) {
    SizeLimit s;
    ghostty_action_size_limit_s limit{};
    limit.min_width = 200;
    limit.min_height = 150;
    s.Apply(limit);

    auto t = s.Clamp(Proposed(0, 0, 9999, 9999));
    EXPECT_EQ(t.minWidth, 200);
    EXPECT_EQ(t.minHeight, 150);
    // Max wasn't populated → proposal preserved.
    EXPECT_EQ(t.maxWidth, 9999);
    EXPECT_EQ(t.maxHeight, 9999);
}

TEST(SizeLimitTest, ClampsMaxTrackSize) {
    SizeLimit s;
    ghostty_action_size_limit_s limit{};
    limit.max_width = 800;
    limit.max_height = 600;
    s.Apply(limit);

    auto t = s.Clamp(Proposed(0, 0, 9999, 9999));
    EXPECT_EQ(t.maxWidth, 800);
    EXPECT_EQ(t.maxHeight, 600);
    EXPECT_EQ(t.minWidth, 0);
    EXPECT_EQ(t.minHeight, 0);
}

TEST(SizeLimitTest, ZeroFieldsLeaveProposalUntouched) {
    SizeLimit s;
    s.Apply(ghostty_action_size_limit_s{});  // all zero — "no override"

    auto t = s.Clamp(Proposed(100, 50, 1000, 500));
    EXPECT_EQ(t.minWidth, 100);
    EXPECT_EQ(t.minHeight, 50);
    EXPECT_EQ(t.maxWidth, 1000);
    EXPECT_EQ(t.maxHeight, 500);
}

TEST(SizeLimitTest, SecondApplyReplacesTheValue) {
    SizeLimit s;
    ghostty_action_size_limit_s first{};
    first.min_width = 100;
    s.Apply(first);
    ghostty_action_size_limit_s second{};
    second.min_width = 300;
    s.Apply(second);

    EXPECT_EQ(s.Clamp(Proposed(0, 0, 9999, 9999)).minWidth, 300);
}

TEST(SizeLimitTest, IsAPlainValue) {
    SizeLimit a;
    ghostty_action_size_limit_s limit{};
    limit.min_width = 100;
    a.Apply(limit);

    SizeLimit b = a;   // NativeWindow keeps its own copy this way
    EXPECT_EQ(b.Clamp(Proposed(0, 0, 9999, 9999)).minWidth, 100);
}
