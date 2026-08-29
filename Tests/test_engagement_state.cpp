#include "pch.h"
#include "../Core/Host/EngagementState.h"

using core::host::EngagementState;
using Action = EngagementState::Action;

TEST(EngagementStateTest, WishBeforeContextIsRememberedAndAppliedOnCreation) {
    EngagementState s;
    EXPECT_EQ(s.Want(true), Action::None);
    EXPECT_TRUE(s.Wants());
    EXPECT_FALSE(s.IsEngaged());

    EXPECT_EQ(s.ContextCreated(), Action::Enter);
    EXPECT_TRUE(s.HasContext());
    EXPECT_TRUE(s.IsEngaged());
}

TEST(EngagementStateTest, ContextWithoutWishDoesNothing) {
    EngagementState s;
    EXPECT_EQ(s.ContextCreated(), Action::None);
    EXPECT_FALSE(s.IsEngaged());
}

TEST(EngagementStateTest, RepeatingTheCurrentStateIsNoop) {
    EngagementState s;
    s.ContextCreated();
    EXPECT_EQ(s.Want(true), Action::Enter);
    EXPECT_EQ(s.Want(true), Action::None);
    EXPECT_EQ(s.Want(false), Action::Leave);
    EXPECT_EQ(s.Want(false), Action::None);
}

TEST(EngagementStateTest, ReleaseGivesBackOnlyWhatWasTaken) {
    EngagementState s;
    s.ContextCreated();
    EXPECT_EQ(s.ContextReleased(), Action::None);   // never engaged: nothing to give back
    EXPECT_FALSE(s.HasContext());

    s.ContextCreated();
    s.Want(true);
    EXPECT_EQ(s.ContextReleased(), Action::Leave);
    EXPECT_FALSE(s.IsEngaged());
    EXPECT_FALSE(s.HasContext());
}

TEST(EngagementStateTest, WishSurvivesRelease) {
    EngagementState s;
    s.Want(true);
    EXPECT_EQ(s.ContextCreated(), Action::Enter);
    EXPECT_EQ(s.ContextReleased(), Action::Leave);
    EXPECT_TRUE(s.Wants());
    EXPECT_EQ(s.ContextCreated(), Action::Enter);   // the next context applies it again
}

TEST(EngagementStateTest, WithdrawingBeforeContextLeavesNothingToApply) {
    EngagementState s;
    s.Want(true);
    s.Want(false);
    EXPECT_EQ(s.ContextCreated(), Action::None);
    EXPECT_FALSE(s.IsEngaged());
}
