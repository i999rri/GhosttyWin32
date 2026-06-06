#include "pch.h"
#include "../Core/Input/KeyEventTranslator.h"

using core::input::RawKeyPress;
using core::input::RawKeyRelease;
using core::input::Translate;

// ----- keycode: raw Win scan code + extended bit, NOT a ghostty enum -----

TEST(KeyEventTranslatorTest, KeycodeIsRawScanCode_F11_Issue60) {
    // ghostty's keycodes.zig table looks up the Win scan code in the
    // platform-native column to derive physical_key. Passing the
    // ghostty enum value instead leaves physical_key = .unidentified
    // and every physical-key binding (f11 = toggle_fullscreen, etc.)
    // silently misses — the regression we're guarding against.
    RawKeyPress raw{
        .vk_code   = VK_F11,
        .scan_code = 0x0057,
    };

    auto out = Translate(raw);
    EXPECT_EQ(0x0057u, out.keycode);
}

TEST(KeyEventTranslatorTest, KeycodeIncludesExtendedBit) {
    // Arrow keys + the right-hand modifiers + the navigation cluster
    // all live in the extended-key range (0xE0xx) in ghostty's
    // keycodes table. The host signals extended via
    // KeyStatus.IsExtendedKey; we OR 0xE000 into keycode so the table
    // hits the correct row.
    RawKeyPress raw{
        .vk_code     = VK_LEFT,
        .scan_code   = 0x004B,
        .is_extended = true,
    };

    auto out = Translate(raw);
    EXPECT_EQ(0xE04Bu, out.keycode);
}

// ----- action: encoded by the static type, not by a runtime param -----

TEST(KeyEventTranslatorTest, PressOverloadProducesActionPress) {
    RawKeyPress raw{};
    auto out = Translate(raw);
    EXPECT_EQ(GHOSTTY_ACTION_PRESS, out.action);
}

TEST(KeyEventTranslatorTest, ReleaseOverloadProducesActionRelease) {
    RawKeyRelease raw{};
    auto out = Translate(raw);
    EXPECT_EQ(GHOSTTY_ACTION_RELEASE, out.action);
}

// ----- mods bitmask -----

TEST(KeyEventTranslatorTest, ModsBitmaskFromIndividualFlags) {
    RawKeyPress raw{
        .ctrl  = true,
        .shift = true,
    };

    auto out = Translate(raw);
    auto expected = static_cast<ghostty_input_mods_e>(
        GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT);
    EXPECT_EQ(expected, out.mods);
}

TEST(KeyEventTranslatorTest, ModsBitmaskRoundTripsOnRelease) {
    // Release events still need correct mods so binding triggers
    // that fire on release (rare but possible) match.
    RawKeyRelease raw{
        .ctrl = true,
        .alt  = true,
    };

    auto out = Translate(raw);
    auto expected = static_cast<ghostty_input_mods_e>(
        GHOSTTY_MODS_CTRL | GHOSTTY_MODS_ALT);
    EXPECT_EQ(expected, out.mods);
}

// ----- consumed_mods stays 0 -----

TEST(KeyEventTranslatorTest, ConsumedModsStaysZero) {
    // Setting consumed_mods to the live modifier value silently
    // suppresses every modifier-based binding (ctrl+c never sends
    // SIGINT, ctrl+shift+t never opens a new tab). Stay at 0 until
    // the host can surface a real "modifiers used to translate"
    // value.
    RawKeyPress raw{
        .ctrl = true,
        .alt  = true,
    };

    auto out = Translate(raw);
    EXPECT_EQ(static_cast<ghostty_input_mods_e>(0), out.consumed_mods);
}

// ----- text passed through verbatim -----

TEST(KeyEventTranslatorTest, TextPassedThroughForPrintable) {
    // Printable 'a': ghostty's encoder writes event.utf8 to the pty
    // via the legacy fallback when no binding matches, so we need
    // text populated to get characters into the terminal.
    const char* a = "a";
    RawKeyPress raw{
        .vk_code             = 'A',
        .scan_code           = 0x001E,
        .text                = a,
        .unshifted_codepoint = 'a',
    };

    auto out = Translate(raw);
    EXPECT_EQ(a, out.text);
    EXPECT_EQ(static_cast<uint32_t>('a'), out.unshifted_codepoint);
    EXPECT_EQ(0x001Eu, out.keycode);
}

TEST(KeyEventTranslatorTest, TextNullForNonPrintable) {
    // Function keys, modifiers, dead keys — text stays null because
    // ghostty's encoder doesn't expect a printable representation
    // for them.
    RawKeyPress raw{
        .scan_code = 0x0057,  // F11
    };

    auto out = Translate(raw);
    EXPECT_EQ(nullptr, out.text);
}

TEST(KeyEventTranslatorTest, ReleaseAlwaysHasNullText) {
    // RawKeyRelease has no `text` field at all — the release
    // overload writes nullptr unconditionally. Locks in the
    // invariant: a release event can never carry text.
    RawKeyRelease raw{
        .vk_code   = 'A',
        .scan_code = 0x001E,
    };

    auto out = Translate(raw);
    EXPECT_EQ(nullptr, out.text);
    EXPECT_EQ(0u, out.unshifted_codepoint);
    EXPECT_FALSE(out.composing);
}

// ----- unshifted_codepoint propagates for unicode bindings -----

TEST(KeyEventTranslatorTest, UnshiftedCodepointPropagates) {
    // `keybind = ctrl+shift+t = new_tab` resolves on the unicode
    // side via unshifted_codepoint == 't'. The translator just
    // propagates the host's pre-computed value through.
    RawKeyPress raw{
        .vk_code             = 'T',
        .scan_code           = 0x0014,
        .shift               = true,
        .ctrl                = true,
        .unshifted_codepoint = 't',
    };

    auto out = Translate(raw);
    EXPECT_EQ(static_cast<uint32_t>('t'), out.unshifted_codepoint);
}

// ----- composing flag propagates -----

TEST(KeyEventTranslatorTest, ComposingFlagPropagates) {
    RawKeyPress raw{
        .composing = true,
    };

    auto out = Translate(raw);
    EXPECT_TRUE(out.composing);
}
