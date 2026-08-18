#include "pch.h"
#include "../App/Tabs/PressedTab.h"

// Same int* instantiation trick as test_tab_drag.cpp.
using winrt::GhosttyWin32::implementation::BasicPressedTab;

namespace {
int a = 0, b = 0;
}

TEST(PressedTabTest, StartsEmpty) {
    BasicPressedTab<int*> p;
    EXPECT_EQ(p.Take(), nullptr);
}

TEST(PressedTabTest, TakeReturnsTheRecordedItemOnce) {
    // One press feeds at most one drag.
    BasicPressedTab<int*> p;
    p.Record(&a);
    EXPECT_EQ(p.Take(), &a);
    EXPECT_EQ(p.Take(), nullptr);
}

TEST(PressedTabTest, NextRecordOverwrites) {
    BasicPressedTab<int*> p;
    p.Record(&a);
    p.Record(&b);
    EXPECT_EQ(p.Take(), &b);
}

TEST(PressedTabTest, ForgetClearsAMatchingItem) {
    // Closing a tab must release the reference a stale press holds.
    BasicPressedTab<int*> p;
    p.Record(&a);
    p.Forget(&a);
    EXPECT_EQ(p.Take(), nullptr);
}

TEST(PressedTabTest, ForgetIgnoresANonMatchingItem) {
    // Closing some other tab must not disturb a live press record.
    BasicPressedTab<int*> p;
    p.Record(&a);
    p.Forget(&b);
    EXPECT_EQ(p.Take(), &a);
}
