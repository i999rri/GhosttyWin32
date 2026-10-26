#include "pch.h"
#include "Panes/Command.h"

using core::panes::Command;

TEST(PaneCommandTest, DefaultSaysNothingAndLeavesTheConfigInCharge) {
    EXPECT_TRUE(Command::Default().IsDefault());
    EXPECT_TRUE(Command::Default().Text().empty());
    EXPECT_FALSE(Command::Of("pwsh").IsDefault());
}

TEST(PaneCommandTest, KeepsTheLineAsWritten) {
    EXPECT_EQ(Command::Of("wsl -d Ubuntu --cd ~/src").Text(), "wsl -d Ubuntu --cd ~/src");
}

TEST(PaneCommandTest, KnowsWslInAnySpellingAShellTakes) {
    EXPECT_TRUE(Command::Of("wsl").StartsWsl());
    EXPECT_TRUE(Command::Of("WSL.EXE").StartsWsl());
    EXPECT_TRUE(Command::Of("wsl -d Ubuntu").StartsWsl());
    EXPECT_TRUE(Command::Of("wsl\t-d Ubuntu").StartsWsl());
}

TEST(PaneCommandTest, DoesNotMistakeAnotherProgramForWsl) {
    EXPECT_FALSE(Command::Default().StartsWsl());
    EXPECT_FALSE(Command::Of("pwsh").StartsWsl());
    EXPECT_FALSE(Command::Of("wslx").StartsWsl());
    EXPECT_FALSE(Command::Of("pwsh -c wsl").StartsWsl());
}

TEST(PaneCommandTest, ComparesByWhatItRuns) {
    EXPECT_EQ(Command::Of("wsl"), Command::Of("wsl"));
    EXPECT_NE(Command::Of("wsl"), Command::Of("wsl -d Ubuntu"));
    EXPECT_EQ(Command::Default(), Command{});
}
