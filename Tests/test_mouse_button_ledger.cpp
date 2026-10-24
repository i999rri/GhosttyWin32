#include "pch.h"
#include "Input/MouseButtonLedger.h"

using core::input::MouseButtonLedger;

TEST(MouseButtonLedgerTest, StartsWithNothingDown) {
    MouseButtonLedger ledger;
    EXPECT_FALSE(ledger.Release(GHOSTTY_MOUSE_LEFT));
}

TEST(MouseButtonLedgerTest, OnlyTheFirstPressIsHeard) {
    MouseButtonLedger ledger;
    EXPECT_TRUE(ledger.Press(GHOSTTY_MOUSE_LEFT));
    EXPECT_FALSE(ledger.Press(GHOSTTY_MOUSE_LEFT));
}

TEST(MouseButtonLedgerTest, ReleaseTakesBackThePress) {
    MouseButtonLedger ledger;
    ledger.Press(GHOSTTY_MOUSE_MIDDLE);
    EXPECT_TRUE(ledger.Release(GHOSTTY_MOUSE_MIDDLE));
    EXPECT_FALSE(ledger.Release(GHOSTTY_MOUSE_MIDDLE));
}

TEST(MouseButtonLedgerTest, ReleaseNeedsThatButtonsOwnPress) {
    // The right-click that copies a selection sends no press, so its
    // release must not be reported either.
    MouseButtonLedger ledger;
    ledger.Press(GHOSTTY_MOUSE_LEFT);
    EXPECT_FALSE(ledger.Release(GHOSTTY_MOUSE_RIGHT));
}

TEST(MouseButtonLedgerTest, ButtonsAreHeldIndependently) {
    // Windows reports five ledger that can all be down at once.
    MouseButtonLedger ledger;
    EXPECT_TRUE(ledger.Press(GHOSTTY_MOUSE_LEFT));
    EXPECT_TRUE(ledger.Press(GHOSTTY_MOUSE_RIGHT));
    EXPECT_TRUE(ledger.Release(GHOSTTY_MOUSE_LEFT));
    EXPECT_TRUE(ledger.Release(GHOSTTY_MOUSE_RIGHT));
}

TEST(MouseButtonLedgerTest, AButtonCanBePressedAgainAfterItsRelease) {
    MouseButtonLedger ledger;
    ledger.Press(GHOSTTY_MOUSE_LEFT);
    ledger.Release(GHOSTTY_MOUSE_LEFT);
    EXPECT_TRUE(ledger.Press(GHOSTTY_MOUSE_LEFT));
}

TEST(MouseButtonLedgerTest, NothingIsOutstandingUntilAPress) {
    MouseButtonLedger ledger;
    EXPECT_FALSE(ledger.AnyOutstanding());
    ledger.Press(GHOSTTY_MOUSE_LEFT);
    EXPECT_TRUE(ledger.AnyOutstanding());
}

TEST(MouseButtonLedgerTest, TheLastReleaseEndsTheCapture) {
    // The host holds the pointer capture while a press is outstanding,
    // so a second button must keep it past the first one's release.
    MouseButtonLedger ledger;
    ledger.Press(GHOSTTY_MOUSE_LEFT);
    ledger.Press(GHOSTTY_MOUSE_RIGHT);
    ledger.Release(GHOSTTY_MOUSE_LEFT);
    EXPECT_TRUE(ledger.AnyOutstanding());
    ledger.Release(GHOSTTY_MOUSE_RIGHT);
    EXPECT_FALSE(ledger.AnyOutstanding());
}

TEST(MouseButtonLedgerTest, ReleaseAnyDrainsEveryHeldButton) {
    // What a lost capture needs: the releases it was going to
    // deliver, all of them, with no repeats.
    MouseButtonLedger ledger;
    ledger.Press(GHOSTTY_MOUSE_LEFT);
    ledger.Press(GHOSTTY_MOUSE_MIDDLE);

    EXPECT_EQ(ledger.ReleaseAny(), GHOSTTY_MOUSE_LEFT);
    EXPECT_EQ(ledger.ReleaseAny(), GHOSTTY_MOUSE_MIDDLE);
    EXPECT_FALSE(ledger.ReleaseAny().has_value());
    EXPECT_FALSE(ledger.AnyOutstanding());
}

TEST(MouseButtonLedgerTest, ReleaseAnyOnAnIdleRecordSaysNothing) {
    // The ordinary release path reaches the drain too, by releasing
    // the capture itself, and must find nothing left to send.
    MouseButtonLedger ledger;
    ledger.Press(GHOSTTY_MOUSE_LEFT);
    ledger.Release(GHOSTTY_MOUSE_LEFT);
    EXPECT_FALSE(ledger.ReleaseAny().has_value());
}

TEST(MouseButtonLedgerTest, TheHighestButtonLibghosttyDefinesFits) {
    // The record is one slot per button, sized from the top of the
    // enum, so that is the index that has to stay in range.
    MouseButtonLedger ledger;
    EXPECT_TRUE(ledger.Press(GHOSTTY_MOUSE_ELEVEN));
    EXPECT_TRUE(ledger.Release(GHOSTTY_MOUSE_ELEVEN));
}
