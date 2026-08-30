#include "pch.h"
#include "../Core/Ghostty/Actions/Tags/Fullscreen.h"

using core::ghostty::actions::tags::Fullscreen;
using Transition = Fullscreen::Transition;

// The value only: which way a toggle goes and whether the window is
// in. Spanning the monitor and restoring the placement are
// NativeWindow's, tested in test_native_window.

TEST(FullscreenTest, StartsOut) {
    Fullscreen f;
    EXPECT_FALSE(f.Active());
}

TEST(FullscreenTest, FirstToggleEnters) {
    Fullscreen f;
    EXPECT_EQ(f.Toggle(), Transition::Enter);
    EXPECT_TRUE(f.Active());
}

TEST(FullscreenTest, SecondToggleLeaves) {
    Fullscreen f;
    f.Toggle();
    EXPECT_EQ(f.Toggle(), Transition::Leave);
    EXPECT_FALSE(f.Active());
}

TEST(FullscreenTest, IsAPlainValue) {
    Fullscreen a;
    a.Toggle();
    Fullscreen b = a;
    EXPECT_TRUE(b.Active());
}
