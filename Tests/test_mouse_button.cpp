#include "pch.h"
#include "Input/MouseButton.h"

using core::input::MouseButton;

TEST(MouseButtonTest, AnswersWhichButtonItIs) {
    EXPECT_TRUE(MouseButton::Left().IsLeft());
    EXPECT_FALSE(MouseButton::Left().IsRight());
    EXPECT_TRUE(MouseButton::Right().IsRight());
    EXPECT_TRUE(MouseButton::Middle().IsMiddle());
}

TEST(MouseButtonTest, CrossesIntoLibghosttyAndBack) {
    // The translation is the only place the two numberings meet, so a
    // round trip through it has to land on the same button.
    for (auto button : { MouseButton::Left(), MouseButton::Right(), MouseButton::Middle() }) {
        auto back = MouseButton::FromGhosttyEnum(button.ToGhosttyEnum());
        ASSERT_TRUE(back.has_value());
        EXPECT_TRUE(*back == button);
    }
}

TEST(MouseButtonTest, HasNoWordForTheButtonsItDoesNotName) {
    EXPECT_FALSE(MouseButton::FromGhosttyEnum(GHOSTTY_MOUSE_FOUR).has_value());
    EXPECT_FALSE(MouseButton::FromGhosttyEnum(GHOSTTY_MOUSE_UNKNOWN).has_value());
}
