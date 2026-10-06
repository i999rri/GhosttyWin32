#include "pch.h"
#include "Input/PressedMouseButtons.h"

using core::input::PressedMouseButtons;

TEST(PressedMouseButtonsTest, StartsWithNothingDown) {
    PressedMouseButtons buttons;
    EXPECT_FALSE(buttons.Release(GHOSTTY_MOUSE_LEFT));
}

TEST(PressedMouseButtonsTest, OnlyTheFirstPressIsHeard) {
    PressedMouseButtons buttons;
    EXPECT_TRUE(buttons.Press(GHOSTTY_MOUSE_LEFT));
    EXPECT_FALSE(buttons.Press(GHOSTTY_MOUSE_LEFT));
}

TEST(PressedMouseButtonsTest, ReleaseTakesBackThePress) {
    PressedMouseButtons buttons;
    buttons.Press(GHOSTTY_MOUSE_MIDDLE);
    EXPECT_TRUE(buttons.Release(GHOSTTY_MOUSE_MIDDLE));
    EXPECT_FALSE(buttons.Release(GHOSTTY_MOUSE_MIDDLE));
}

TEST(PressedMouseButtonsTest, ReleaseNeedsThatButtonsOwnPress) {
    // The right-click that copies a selection sends no press, so its
    // release must not be reported either.
    PressedMouseButtons buttons;
    buttons.Press(GHOSTTY_MOUSE_LEFT);
    EXPECT_FALSE(buttons.Release(GHOSTTY_MOUSE_RIGHT));
}

TEST(PressedMouseButtonsTest, ButtonsAreHeldIndependently) {
    // Windows reports five buttons that can all be down at once.
    PressedMouseButtons buttons;
    EXPECT_TRUE(buttons.Press(GHOSTTY_MOUSE_LEFT));
    EXPECT_TRUE(buttons.Press(GHOSTTY_MOUSE_RIGHT));
    EXPECT_TRUE(buttons.Release(GHOSTTY_MOUSE_LEFT));
    EXPECT_TRUE(buttons.Release(GHOSTTY_MOUSE_RIGHT));
}

TEST(PressedMouseButtonsTest, AButtonCanBePressedAgainAfterItsRelease) {
    PressedMouseButtons buttons;
    buttons.Press(GHOSTTY_MOUSE_LEFT);
    buttons.Release(GHOSTTY_MOUSE_LEFT);
    EXPECT_TRUE(buttons.Press(GHOSTTY_MOUSE_LEFT));
}

TEST(PressedMouseButtonsTest, NothingIsDownUntilAButtonIsPressed) {
    PressedMouseButtons buttons;
    EXPECT_FALSE(buttons.AnyDown());
    buttons.Press(GHOSTTY_MOUSE_LEFT);
    EXPECT_TRUE(buttons.AnyDown());
}

TEST(PressedMouseButtonsTest, TheLastReleaseEndsTheCapture) {
    // The host holds the pointer capture while AnyDown, so a second
    // button must keep it past the first one's release.
    PressedMouseButtons buttons;
    buttons.Press(GHOSTTY_MOUSE_LEFT);
    buttons.Press(GHOSTTY_MOUSE_RIGHT);
    buttons.Release(GHOSTTY_MOUSE_LEFT);
    EXPECT_TRUE(buttons.AnyDown());
    buttons.Release(GHOSTTY_MOUSE_RIGHT);
    EXPECT_FALSE(buttons.AnyDown());
}

TEST(PressedMouseButtonsTest, ReleaseAnyDrainsEveryHeldButton) {
    // What a lost capture needs: the releases it was going to
    // deliver, all of them, with no repeats.
    PressedMouseButtons buttons;
    buttons.Press(GHOSTTY_MOUSE_LEFT);
    buttons.Press(GHOSTTY_MOUSE_MIDDLE);

    EXPECT_EQ(buttons.ReleaseAny(), GHOSTTY_MOUSE_LEFT);
    EXPECT_EQ(buttons.ReleaseAny(), GHOSTTY_MOUSE_MIDDLE);
    EXPECT_FALSE(buttons.ReleaseAny().has_value());
    EXPECT_FALSE(buttons.AnyDown());
}

TEST(PressedMouseButtonsTest, ReleaseAnyOnAnIdleRecordSaysNothing) {
    // The ordinary release path reaches the drain too, by releasing
    // the capture itself, and must find nothing left to send.
    PressedMouseButtons buttons;
    buttons.Press(GHOSTTY_MOUSE_LEFT);
    buttons.Release(GHOSTTY_MOUSE_LEFT);
    EXPECT_FALSE(buttons.ReleaseAny().has_value());
}

TEST(PressedMouseButtonsTest, TheHighestButtonLibghosttyDefinesFits) {
    // The record is one bit per button, so the top of the enum is
    // the shift that has to stay in range.
    PressedMouseButtons buttons;
    EXPECT_TRUE(buttons.Press(GHOSTTY_MOUSE_ELEVEN));
    EXPECT_TRUE(buttons.Release(GHOSTTY_MOUSE_ELEVEN));
}
