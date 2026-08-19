#include "pch.h"
#include "../Core/Ghostty/Actions/Tags/TriggerLabel.h"

using core::ghostty::actions::TriggerLabel;

namespace {

ghostty_input_trigger_s UnicodeTrigger(uint32_t cp, int mods) {
    ghostty_input_trigger_s t{};
    t.tag = GHOSTTY_TRIGGER_UNICODE;
    t.key.unicode = cp;
    t.mods = static_cast<ghostty_input_mods_e>(mods);
    return t;
}

}  // namespace

TEST(TriggerLabelTest, UnicodeKeyWithMods) {
    EXPECT_EQ(TriggerLabel(UnicodeTrigger(U'a', GHOSTTY_MODS_CTRL)),
              L"ctrl+a");
    EXPECT_EQ(TriggerLabel(UnicodeTrigger(U'b',
                                          GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT)),
              L"ctrl+shift+b");
}

TEST(TriggerLabelTest, ModOrderIsCtrlAltShiftSuper) {
    EXPECT_EQ(TriggerLabel(UnicodeTrigger(U'x',
                                          GHOSTTY_MODS_SUPER | GHOSTTY_MODS_SHIFT |
                                          GHOSTTY_MODS_ALT | GHOSTTY_MODS_CTRL)),
              L"ctrl+alt+shift+super+x");
}

TEST(TriggerLabelTest, NoModsIsBareKey) {
    EXPECT_EQ(TriggerLabel(UnicodeTrigger(U'g', GHOSTTY_MODS_NONE)), L"g");
}

TEST(TriggerLabelTest, PhysicalKeyUsesConfigSyntaxName) {
    ghostty_input_trigger_s t{};
    t.tag = GHOSTTY_TRIGGER_PHYSICAL;
    t.key.physical = GHOSTTY_KEY_BRACKET_LEFT;
    t.mods = GHOSTTY_MODS_CTRL;
    EXPECT_EQ(TriggerLabel(t), L"ctrl+physical:bracket_left");
}

TEST(TriggerLabelTest, UnknownPhysicalKeyFallsBack) {
    ghostty_input_trigger_s t{};
    t.tag = GHOSTTY_TRIGGER_PHYSICAL;
    t.key.physical = static_cast<ghostty_input_key_e>(9999);
    EXPECT_EQ(TriggerLabel(t), L"physical:unidentified");
}

TEST(TriggerLabelTest, AstralCodepointBecomesSurrogatePair) {
    // U+1F512 (the padlock) needs a UTF-16 surrogate pair; a naive
    // single-wchar cast would produce a garbage character.
    auto label = TriggerLabel(UnicodeTrigger(0x1F512, GHOSTTY_MODS_NONE));
    ASSERT_EQ(label.size(), 2u);
    EXPECT_EQ(label[0], wchar_t{0xD83D});
    EXPECT_EQ(label[1], wchar_t{0xDD12});
}
