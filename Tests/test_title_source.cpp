#include "pch.h"
#include "../Core/Host/TitleSource.h"

using core::host::TitleSource;

TEST(TitleSourceTest, RanksUserOverShellOverAutomatic) {
    EXPECT_TRUE(TitleSource::User().Outranks(TitleSource::Shell()));
    EXPECT_TRUE(TitleSource::User().Outranks(TitleSource::Automatic()));
    EXPECT_TRUE(TitleSource::Shell().Outranks(TitleSource::Automatic()));

    EXPECT_FALSE(TitleSource::Automatic().Outranks(TitleSource::Shell()));
    EXPECT_FALSE(TitleSource::Automatic().Outranks(TitleSource::User()));
    EXPECT_FALSE(TitleSource::Shell().Outranks(TitleSource::User()));
}

TEST(TitleSourceTest, EqualRankMayReassert) {
    // The shell re-sends its OSC title on every prompt and the poll
    // refreshes its own automatic name — an equal writer never blocks.
    EXPECT_FALSE(TitleSource::Automatic().Outranks(TitleSource::Automatic()));
    EXPECT_FALSE(TitleSource::Shell().Outranks(TitleSource::Shell()));
    EXPECT_FALSE(TitleSource::User().Outranks(TitleSource::User()));
}

TEST(TitleSourceTest, AnswersItsIdentity) {
    EXPECT_TRUE(TitleSource::Automatic().IsAutomatic());
    EXPECT_TRUE(TitleSource::Shell().IsShell());
    EXPECT_TRUE(TitleSource::User().IsUser());

    EXPECT_FALSE(TitleSource::Automatic().IsShell());
    EXPECT_FALSE(TitleSource::Automatic().IsUser());
    EXPECT_FALSE(TitleSource::Shell().IsAutomatic());

    EXPECT_EQ(TitleSource::Shell(), TitleSource::Shell());
    EXPECT_NE(TitleSource::Shell(), TitleSource::User());
}
