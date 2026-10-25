#include "pch.h"
#include "Input/MouseTransition.h"

using core::input::ButtonReport;
using core::input::MouseButton;
using core::input::MouseButtonLedger;
using core::input::MouseTransition;
using Capture = core::input::MouseTransition::Capture;

namespace {

MouseButtonLedger Holding(std::initializer_list<MouseButton> buttons) {
    MouseButtonLedger ledger;
    for (auto b : buttons) ledger.Press(b.ToGhosttyEnum());
    return ledger;
}

ButtonReport Press(MouseButton b) { return { b, GHOSTTY_MOUSE_PRESS }; }
ButtonReport Release(MouseButton b) { return { b, GHOSTTY_MOUSE_RELEASE }; }

}  // namespace

// ----- press -----

TEST(MouseTransitionTest, FirstPressIsReportedAndTakesTheCapture) {
    auto t = core::input::OnPress(MouseButtonLedger{}, MouseButton::Left(), false);
    EXPECT_TRUE(t.ledger.IsOutstanding(GHOSTTY_MOUSE_LEFT));
    EXPECT_EQ(t.capture, Capture::Take);
    EXPECT_EQ(t.reports, std::vector{ Press(MouseButton::Left()) });
    EXPECT_FALSE(t.copySelection);
}

TEST(MouseTransitionTest, SecondButtonIsReportedButTheCaptureIsAlreadyHeld) {
    auto t = core::input::OnPress(Holding({ MouseButton::Left() }), MouseButton::Right(), false);
    EXPECT_TRUE(t.ledger.IsOutstanding(GHOSTTY_MOUSE_RIGHT));
    EXPECT_EQ(t.capture, Capture::Keep);
    EXPECT_EQ(t.reports, std::vector{ Press(MouseButton::Right()) });
}

TEST(MouseTransitionTest, APressLibghosttyHasHeardChangesNothing) {
    auto before = Holding({ MouseButton::Left() });
    auto t = core::input::OnPress(before, MouseButton::Left(), false);
    EXPECT_TRUE(t.ledger.IsOutstanding(GHOSTTY_MOUSE_LEFT));
    EXPECT_EQ(t.capture, Capture::Keep);
    EXPECT_TRUE(t.reports.empty());
    EXPECT_FALSE(t.copySelection);
}

TEST(MouseTransitionTest, RightClickOverASelectionCopiesAndReportsNothing) {
    auto t = core::input::OnPress(MouseButtonLedger{}, MouseButton::Right(), true);
    EXPECT_TRUE(t.copySelection);
    EXPECT_TRUE(t.reports.empty());
    EXPECT_FALSE(t.ledger.AnyOutstanding());
    EXPECT_EQ(t.capture, Capture::Keep);
}

TEST(MouseTransitionTest, RightClickWithoutASelectionIsRelayed) {
    auto t = core::input::OnPress(MouseButtonLedger{}, MouseButton::Right(), false);
    EXPECT_FALSE(t.copySelection);
    EXPECT_EQ(t.reports, std::vector{ Press(MouseButton::Right()) });
    EXPECT_EQ(t.capture, Capture::Take);
}

TEST(MouseTransitionTest, RightClickUnderAHeldLeftIsRelayedEvenOverASelection) {
    // Copying would clear the selection the left button is drawing.
    auto t = core::input::OnPress(Holding({ MouseButton::Left() }), MouseButton::Right(), true);
    EXPECT_FALSE(t.copySelection);
    EXPECT_EQ(t.reports, std::vector{ Press(MouseButton::Right()) });
}

// ----- release -----

TEST(MouseTransitionTest, ReleasingTheLastButtonReportsItAndDropsTheCapture) {
    auto t = core::input::OnRelease(Holding({ MouseButton::Left() }), MouseButton::Left());
    EXPECT_FALSE(t.ledger.AnyOutstanding());
    EXPECT_EQ(t.reports, std::vector{ Release(MouseButton::Left()) });
    EXPECT_EQ(t.capture, Capture::Drop);
}

TEST(MouseTransitionTest, ReleasingOneOfTwoKeepsTheCapture) {
    auto t = core::input::OnRelease(Holding({ MouseButton::Left(), MouseButton::Right() }), MouseButton::Left());
    EXPECT_TRUE(t.ledger.IsOutstanding(GHOSTTY_MOUSE_RIGHT));
    EXPECT_EQ(t.reports, std::vector{ Release(MouseButton::Left()) });
    EXPECT_EQ(t.capture, Capture::Keep);
}

TEST(MouseTransitionTest, AReleaseWithoutItsPressReportsNothing) {
    // The right-click that copied a selection sent no press, so its
    // release is not libghostty's to hear. Nothing is outstanding, so
    // the capture -- which was never taken -- is dropped all the same.
    auto t = core::input::OnRelease(MouseButtonLedger{}, MouseButton::Right());
    EXPECT_TRUE(t.reports.empty());
    EXPECT_EQ(t.capture, Capture::Drop);
}

// ----- capture lost / canceled -----

TEST(MouseTransitionTest, LosingTheCaptureTakesBackEveryOutstandingPress) {
    auto t = core::input::OnCaptureLost(Holding({ MouseButton::Left(), MouseButton::Middle() }));
    EXPECT_FALSE(t.ledger.AnyOutstanding());
    EXPECT_EQ(t.reports, (std::vector{ Release(MouseButton::Left()), Release(MouseButton::Middle()) }));
    EXPECT_EQ(t.capture, Capture::Keep);
}

TEST(MouseTransitionTest, LosingTheCaptureWithNothingOutstandingIsEmpty) {
    // The release path reaches this too, by its own capture release,
    // and must find nothing to do.
    auto t = core::input::OnCaptureLost(MouseButtonLedger{});
    EXPECT_TRUE(t.reports.empty());
    EXPECT_EQ(t.capture, Capture::Keep);
}

TEST(MouseTransitionTest, ACanceledPointerAlsoDropsTheCapture) {
    auto t = core::input::OnCanceled(Holding({ MouseButton::Left() }));
    EXPECT_EQ(t.reports, std::vector{ Release(MouseButton::Left()) });
    EXPECT_EQ(t.capture, Capture::Drop);
}
