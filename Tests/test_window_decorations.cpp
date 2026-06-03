#include "pch.h"
#include "../Core/Ghostty/Actions/Tags/WindowDecorations.h"

using core::ghostty::actions::tags::WindowDecorations;

// ----- initial state -----

TEST(WindowDecorationsTest, DefaultsToNoOverride) {
    WindowDecorations d;
    EXPECT_FALSE(d.HasOverride());
    // With no override, effective state mirrors the config.
    EXPECT_TRUE(d.Effective(true));
    EXPECT_FALSE(d.Effective(false));
}

// ----- toggle behaviour: matches upstream GTK 3-state semantics -----

TEST(WindowDecorationsTest, ToggleFromConfigDecoratedForcesUndecorated) {
    WindowDecorations d;
    bool now = d.Toggle(/*configDecorated=*/true);
    EXPECT_FALSE(now);
    EXPECT_TRUE(d.HasOverride());
    // Effective stays false regardless of what config says next —
    // the override wins until cleared.
    EXPECT_FALSE(d.Effective(true));
    EXPECT_FALSE(d.Effective(false));
}

TEST(WindowDecorationsTest, ToggleFromConfigUndecoratedForcesDecorated) {
    WindowDecorations d;
    bool now = d.Toggle(/*configDecorated=*/false);
    EXPECT_TRUE(now);
    EXPECT_TRUE(d.HasOverride());
    EXPECT_TRUE(d.Effective(true));
    EXPECT_TRUE(d.Effective(false));
}

TEST(WindowDecorationsTest, SecondToggleClearsOverride) {
    WindowDecorations d;
    d.Toggle(/*configDecorated=*/true);   // → ForceUndecorated
    bool now = d.Toggle(/*configDecorated=*/true);  // → cleared
    EXPECT_TRUE(now);
    EXPECT_FALSE(d.HasOverride());
    // After clearing, effective falls back to whatever the current
    // config says.
    EXPECT_TRUE(d.Effective(true));
    EXPECT_FALSE(d.Effective(false));
}

TEST(WindowDecorationsTest, SecondToggleClearsForceDecoratedToo) {
    WindowDecorations d;
    d.Toggle(/*configDecorated=*/false);  // → ForceDecorated
    bool now = d.Toggle(/*configDecorated=*/false);  // → cleared
    EXPECT_FALSE(now);
    EXPECT_FALSE(d.HasOverride());
}

// ----- config-change tolerance -----

TEST(WindowDecorationsTest, OverrideSurvivesConfigDefaultFlipping) {
    // Simulates: user toggled the override, then CONFIG_CHANGE
    // arrives with a different `window-decoration` value. Per the
    // tag's contract (and upstream GTK's behaviour), the override
    // stays in place until the user toggles again.
    WindowDecorations d;
    d.Toggle(/*configDecorated=*/true);  // → ForceUndecorated
    // Config later flips to "undecorated by default" — override
    // still wins (it's also undecorated, but the point is the
    // tag doesn't auto-clear).
    EXPECT_FALSE(d.Effective(/*configDecorated=*/false));
    EXPECT_TRUE(d.HasOverride());
}

// ----- third toggle restores the flipped-from-config behaviour -----

TEST(WindowDecorationsTest, ThirdToggleFlipsAgainstCurrentConfig) {
    // After clearing the override, the next toggle behaves as if it
    // were the first: flip against whatever the config currently says.
    WindowDecorations d;
    d.Toggle(/*configDecorated=*/true);   // ForceUndecorated
    d.Toggle(/*configDecorated=*/true);   // cleared
    bool now = d.Toggle(/*configDecorated=*/false);  // config flipped
    EXPECT_TRUE(now);  // flip against the (now-undecorated) config
}
