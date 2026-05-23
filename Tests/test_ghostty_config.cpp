#include "pch.h"
#include "../App/GhosttyConfig.h"

using winrt::GhosttyWin32::implementation::GhosttyConfig;

// ----- Darken -----
// Linear per-channel multiply by (1 - factor). No gamma; the
// upstream macOS `OSColor.darken(by:)` doesn't do any either, and
// the divider-fallback contract is "stay byte-identical to upstream".

TEST(GhosttyConfigTest, DarkenByZeroIsIdentity) {
    ghostty_config_color_s in{ 100, 150, 200 };
    auto out = GhosttyConfig::Darken(in, 0.0);
    EXPECT_EQ(out.r, 100);
    EXPECT_EQ(out.g, 150);
    EXPECT_EQ(out.b, 200);
}

TEST(GhosttyConfigTest, DarkenByOneIsBlack) {
    auto out = GhosttyConfig::Darken({ 255, 255, 255 }, 1.0);
    EXPECT_EQ(out.r, 0);
    EXPECT_EQ(out.g, 0);
    EXPECT_EQ(out.b, 0);
}

TEST(GhosttyConfigTest, DarkenByHalfHalvesEachChannel) {
    auto out = GhosttyConfig::Darken({ 200, 100, 50 }, 0.5);
    EXPECT_EQ(out.r, 100);
    EXPECT_EQ(out.g, 50);
    EXPECT_EQ(out.b, 25);
}

TEST(GhosttyConfigTest, DarkenLeavesZerosAtZero) {
    auto out = GhosttyConfig::Darken({ 0, 0, 0 }, 0.4);
    EXPECT_EQ(out.r, 0);
    EXPECT_EQ(out.g, 0);
    EXPECT_EQ(out.b, 0);
}

// ----- IsLight -----
// Classification = channel-average > 128. Upstream uses the same
// boundary to pick 8% vs 40% darken in the divider fallback.

TEST(GhosttyConfigTest, IsLightDetectsWhite) {
    EXPECT_TRUE(GhosttyConfig::IsLight({ 255, 255, 255 }));
}

TEST(GhosttyConfigTest, IsLightDetectsBlack) {
    EXPECT_FALSE(GhosttyConfig::IsLight({ 0, 0, 0 }));
}

TEST(GhosttyConfigTest, IsLightBoundaryIsExclusive) {
    // average == 128 must NOT count as light (upstream uses `> 128`).
    EXPECT_FALSE(GhosttyConfig::IsLight({ 128, 128, 128 }));
    // average == 129 (e.g. 130/128/129) -> light.
    EXPECT_TRUE(GhosttyConfig::IsLight({ 130, 128, 129 }));
}

TEST(GhosttyConfigTest, IsLightUsesChannelAverage) {
    // One bright channel + two dark ones can still average dark.
    EXPECT_FALSE(GhosttyConfig::IsLight({ 255, 50, 50 }));   // avg = 118
    // Conversely two bright + one dark can still average light.
    EXPECT_TRUE(GhosttyConfig::IsLight({ 200, 200, 60 }));   // avg = 153
}

// ----- DeriveDividerFromBackground -----
// Light bg -> darken 8%, dark bg -> darken 40%. End result is an
// opaque ARGB colour so the splitter is a clean hairline rather
// than a translucent smear over the surface beneath.

TEST(GhosttyConfigTest, DividerFromLightBackgroundDarkensBy8Percent) {
    // Background: pure white. 8% darken => 255 * 0.92 = 234.6 -> 234.
    auto out = GhosttyConfig::DeriveDividerFromBackground({ 255, 255, 255 });
    EXPECT_EQ(out.A, 255);
    EXPECT_EQ(out.R, 234);
    EXPECT_EQ(out.G, 234);
    EXPECT_EQ(out.B, 234);
}

TEST(GhosttyConfigTest, DividerFromDarkBackgroundDarkensBy40Percent) {
    // Background: 1e1e1e (the project's default dark theme bg). 40%
    // darken => 30 * 0.6 = 18 -> 18.
    auto out = GhosttyConfig::DeriveDividerFromBackground({ 0x1e, 0x1e, 0x1e });
    EXPECT_EQ(out.A, 255);
    EXPECT_EQ(out.R, 18);
    EXPECT_EQ(out.G, 18);
    EXPECT_EQ(out.B, 18);
}

TEST(GhosttyConfigTest, DividerIsAlwaysOpaque) {
    // Alpha must be 255 regardless of input; the splitter's
    // hairline relies on full opacity to read as a clean edge.
    auto fromLight = GhosttyConfig::DeriveDividerFromBackground({ 240, 240, 240 });
    auto fromDark  = GhosttyConfig::DeriveDividerFromBackground({ 30, 30, 30 });
    EXPECT_EQ(fromLight.A, 255);
    EXPECT_EQ(fromDark.A,  255);
}

// ----- ColorFrom -----
// RGB -> ARGB with forced alpha=255. The host applies its own
// translucency where needed (e.g. UnfocusedDim's Opacity).

TEST(GhosttyConfigTest, ColorFromCopiesChannelsAndForcesAlpha) {
    auto out = GhosttyConfig::ColorFrom({ 0x12, 0x34, 0x56 });
    EXPECT_EQ(out.A, 255);
    EXPECT_EQ(out.R, 0x12);
    EXPECT_EQ(out.G, 0x34);
    EXPECT_EQ(out.B, 0x56);
}
