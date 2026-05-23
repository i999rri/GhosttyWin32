#include "pch.h"
#include "../GhosttyWin32App/KeyModifiers.h"

TEST(KeyModifiersTest, NoKeys) {
    auto mods = buildMods(false, false, false);
    EXPECT_EQ(mods, 0);
}

TEST(KeyModifiersTest, ShiftOnly) {
    auto mods = buildMods(true, false, false);
    EXPECT_EQ(mods, GHOSTTY_MODS_SHIFT);
}

TEST(KeyModifiersTest, CtrlOnly) {
    auto mods = buildMods(false, true, false);
    EXPECT_EQ(mods, GHOSTTY_MODS_CTRL);
}

TEST(KeyModifiersTest, AltOnly) {
    auto mods = buildMods(false, false, true);
    EXPECT_EQ(mods, GHOSTTY_MODS_ALT);
}

TEST(KeyModifiersTest, ShiftCtrl) {
    auto mods = buildMods(true, true, false);
    EXPECT_EQ(mods, GHOSTTY_MODS_SHIFT | GHOSTTY_MODS_CTRL);
}

TEST(KeyModifiersTest, AllModifiers) {
    auto mods = buildMods(true, true, true);
    EXPECT_EQ(mods, GHOSTTY_MODS_SHIFT | GHOSTTY_MODS_CTRL | GHOSTTY_MODS_ALT);
}
