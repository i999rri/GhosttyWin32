#include "pch.h"
#include "../App/Tabs/TabDrag.h"

// Instantiated with int* — the template only needs a null-testable,
// assignable item type, which is exactly what lets these tests run
// without the WinUI projection. Production uses TabViewItem.
using winrt::GhosttyWin32::implementation::BasicTabDrag;

namespace {
int a = 0, b = 0;
}

// ----- lifecycle: Begin / End -----

TEST(TabDragTest, StartsIdle) {
    BasicTabDrag<int*> d;
    EXPECT_FALSE(d.InFlight());
    EXPECT_EQ(d.DraggedTab(), nullptr);
    EXPECT_EQ(d.TakeLastDraggedTab(), nullptr);
}

TEST(TabDragTest, BeginEntersFlightAndExposesItem) {
    BasicTabDrag<int*> d;
    d.Begin(&a);
    EXPECT_TRUE(d.InFlight());
    EXPECT_EQ(d.DraggedTab(), &a);
}

TEST(TabDragTest, EndLeavesFlightButKeepsIdentityForTake) {
    // The tear-out consumer runs after TabDragCompleted — the
    // identity must survive End.
    BasicTabDrag<int*> d;
    d.Begin(&a);
    d.End();
    EXPECT_FALSE(d.InFlight());
    EXPECT_EQ(d.DraggedTab(), nullptr);
    EXPECT_EQ(d.TakeLastDraggedTab(), &a);
}

TEST(TabDragTest, TakeIsOneShot) {
    // One drag tears out at most one tab; a second take must not
    // hand the same identity out again.
    BasicTabDrag<int*> d;
    d.Begin(&a);
    d.End();
    EXPECT_EQ(d.TakeLastDraggedTab(), &a);
    EXPECT_EQ(d.TakeLastDraggedTab(), nullptr);
}

// ----- null Begin: unidentified drag -----

TEST(TabDragTest, BeginWithNullDoesNotEnterFlight) {
    // The merge gate (InFlight) must stay closed when the dragged
    // tab could not be identified.
    BasicTabDrag<int*> d;
    d.Begin(nullptr);
    EXPECT_FALSE(d.InFlight());
    EXPECT_EQ(d.DraggedTab(), nullptr);
    EXPECT_EQ(d.TakeLastDraggedTab(), nullptr);
}

TEST(TabDragTest, BeginWithNullClearsPreviousIdentity) {
    // A stale identity from the previous drag must not leak into a
    // new drag whose tab is unknown.
    BasicTabDrag<int*> d;
    d.Begin(&a);
    d.End();
    d.Begin(nullptr);
    EXPECT_EQ(d.TakeLastDraggedTab(), nullptr);
}

// ----- successive drags -----

TEST(TabDragTest, NextBeginOverwritesIdentity) {
    BasicTabDrag<int*> d;
    d.Begin(&a);
    d.End();
    d.Begin(&b);
    EXPECT_TRUE(d.InFlight());
    EXPECT_EQ(d.DraggedTab(), &b);
    d.End();
    EXPECT_EQ(d.TakeLastDraggedTab(), &b);
}

TEST(TabDragTest, EndIsIdempotent) {
    // TabDragCompleted could in principle fire on paths we don't
    // anticipate; a second End must not disturb the kept identity.
    BasicTabDrag<int*> d;
    d.Begin(&a);
    d.End();
    d.End();
    EXPECT_FALSE(d.InFlight());
    EXPECT_EQ(d.TakeLastDraggedTab(), &a);
}
