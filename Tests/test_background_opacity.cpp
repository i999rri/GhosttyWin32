#include "pch.h"
#include "../Core/Ghostty/Actions/Tags/BackgroundOpacity.h"

using core::ghostty::actions::tags::BackgroundOpacity;
using Backdrop = BackgroundOpacity::Backdrop;

// ----- initial state -----

TEST(BackgroundOpacityTest, LaunchesAsConfigured) {
    BackgroundOpacity o;
    EXPECT_FALSE(o.Opaque());
}

// ----- toggle guards: match upstream macOS toggleBackgroundOpacity -----

TEST(BackgroundOpacityTest, ToggleIsNoOpWhenConfigIsOpaque) {
    BackgroundOpacity o;
    EXPECT_FALSE(o.Toggle(/*configOpacity=*/1.0, /*fullscreen=*/false));
    EXPECT_FALSE(o.Opaque());
}

TEST(BackgroundOpacityTest, ToggleIsNoOpWhileFullscreen) {
    BackgroundOpacity o;
    EXPECT_FALSE(o.Toggle(/*configOpacity=*/0.8, /*fullscreen=*/true));
    EXPECT_FALSE(o.Opaque());
}

TEST(BackgroundOpacityTest, ToggleFlipsAndFlipsBack) {
    BackgroundOpacity o;
    EXPECT_TRUE(o.Toggle(0.8, false));
    EXPECT_TRUE(o.Opaque());
    EXPECT_TRUE(o.Toggle(0.8, false));
    EXPECT_FALSE(o.Opaque());
}

// ----- appearance: the three backdrops and what each one needs -----

TEST(BackgroundOpacityTest, OpaqueConfigIsMicaWithPaintedRoot) {
    BackgroundOpacity o;
    auto a = o.Effective(/*configOpacity=*/1.0, /*configBlur=*/true);
    EXPECT_EQ(a.backdrop, Backdrop::Mica);
    EXPECT_FALSE(a.dwmPerPixelAlpha);
    EXPECT_TRUE(a.paintRoot);
    EXPECT_FALSE(a.paneUnderlay);
}

TEST(BackgroundOpacityTest, TranslucentWithoutBlurIsCrispAndNeedsDwmAlpha) {
    BackgroundOpacity o;
    auto a = o.Effective(0.8, /*configBlur=*/false);
    EXPECT_EQ(a.backdrop, Backdrop::Transparent);
    EXPECT_TRUE(a.dwmPerPixelAlpha);
    EXPECT_FALSE(a.paintRoot);
    EXPECT_FALSE(a.paneUnderlay);
}

TEST(BackgroundOpacityTest, TranslucentWithBlurIsClearAcrylic) {
    BackgroundOpacity o;
    auto a = o.Effective(0.8, /*configBlur=*/true);
    EXPECT_EQ(a.backdrop, Backdrop::ClearAcrylic);
    // Acrylic is a DWM material; it composites on its own.
    EXPECT_FALSE(a.dwmPerPixelAlpha);
    EXPECT_FALSE(a.paintRoot);
    EXPECT_FALSE(a.paneUnderlay);
}

TEST(BackgroundOpacityTest, ToggledOpaqueIsMicaWithUnderlays) {
    BackgroundOpacity o;
    o.Toggle(0.8, false);
    auto a = o.Effective(0.8, /*configBlur=*/true);
    EXPECT_EQ(a.backdrop, Backdrop::Mica);
    EXPECT_FALSE(a.dwmPerPixelAlpha);
    EXPECT_TRUE(a.paintRoot);
    // The one case the underlays earn their cost: the swap chain is
    // translucent but the user wants it opaque.
    EXPECT_TRUE(a.paneUnderlay);
}

// ----- config-change tolerance -----

TEST(BackgroundOpacityTest, ReloadToOpaqueConfigDropsUnderlaysButKeepsOverride) {
    // User toggled opaque, then a config reload sets
    // background-opacity = 1.0. The swap chain is opaque on its own
    // now, so no underlay; the override is kept for when the config
    // goes translucent again (matches the previous MainWindow
    // behaviour — nothing ever cleared the flag).
    BackgroundOpacity o;
    o.Toggle(0.8, false);
    auto a = o.Effective(1.0, false);
    EXPECT_EQ(a.backdrop, Backdrop::Mica);
    EXPECT_FALSE(a.paneUnderlay);
    EXPECT_TRUE(o.Opaque());

    auto again = o.Effective(0.8, false);
    EXPECT_TRUE(again.paneUnderlay);
}

TEST(BackgroundOpacityTest, ReloadFlippingBlurSwitchesBackdropInPlace) {
    BackgroundOpacity o;
    EXPECT_EQ(o.Effective(0.8, false).backdrop, Backdrop::Transparent);
    EXPECT_EQ(o.Effective(0.8, true).backdrop, Backdrop::ClearAcrylic);
}
