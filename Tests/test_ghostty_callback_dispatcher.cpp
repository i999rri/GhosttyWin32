#include "pch.h"
#include "MockMainWindowView.h"
#include "../App/GhosttyCallbackDispatcher.h"

using winrt::GhosttyWin32::implementation::GhosttyCallbackDispatcher;

namespace {

// Helper: build an empty target/action pair, set the tag, dispatch.
// The ack-only routing doesn't read target.target or action.action,
// so leaving them default-zeroed is safe.
bool DispatchTag(GhosttyCallbackDispatcher& d, ghostty_action_tag_e tag) {
    ghostty_target_s target{};
    ghostty_action_s action{};
    action.tag = tag;
    return d.DispatchAction(target, action);
}

}  // namespace

// ----- ack-only group -----
// Every tag in the dispatcher's ack-only branch must return true
// (libghostty treats false as "unhandled" and will eventually log
// it). The mock's view methods are all no-ops, so a regression
// that promotes a tag from ack to a real handler wouldn't fail
// these assertions on its own — but it would change behaviour
// elsewhere; the value here is documenting which tags belong in
// the "intentionally silent" set.

TEST(GhosttyCallbackDispatcherTest, InformationalAcksReturnTrue) {
    MockMainWindowView view;
    auto d = GhosttyCallbackDispatcher::Create(view);

    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_READONLY));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_SECURE_INPUT));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_KEY_SEQUENCE));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_KEY_TABLE));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_PROMPT_TITLE));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_PWD));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_COMMAND_FINISHED));
}

TEST(GhosttyCallbackDispatcherTest, FeatureSurfaceAcksReturnTrue) {
    MockMainWindowView view;
    auto d = GhosttyCallbackDispatcher::Create(view);

    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_UNDO));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_REDO));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_START_SEARCH));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_END_SEARCH));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_SEARCH_TOTAL));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_SEARCH_SELECTED));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_INSPECTOR));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_RENDER_INSPECTOR));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_TOGGLE_TAB_OVERVIEW));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_TOGGLE_QUICK_TERMINAL));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_TOGGLE_COMMAND_PALETTE));
}

TEST(GhosttyCallbackDispatcherTest, NoConsumerAcksReturnTrue) {
    MockMainWindowView view;
    auto d = GhosttyCallbackDispatcher::Create(view);

    // SCROLLBAR: no scrollbar UI to feed.
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_SCROLLBAR));
    // QUIT_TIMER: macOS-only quit countdown.
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_QUIT_TIMER));
    // CELL_SIZE: cached but no host-side consumer yet.
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_CELL_SIZE));
}

TEST(GhosttyCallbackDispatcherTest, DisabledFeaturesAckedNotDropped) {
    MockMainWindowView view;
    auto d = GhosttyCallbackDispatcher::Create(view);

    // MOUSE_OVER_LINK: TOOLTIPS popup disabled per #61.
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_MOUSE_OVER_LINK));
    // FLOAT_WINDOW: dispatch path not understood yet, but tag
    // routes to ack so the future re-enable doesn't surprise us.
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_FLOAT_WINDOW));
    // MOUSE_VISIBILITY: disabled per #60.
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_MOUSE_VISIBILITY));
}

// ----- view-free live handlers -----

TEST(GhosttyCallbackDispatcherTest, RingBellDispatchesWithoutTouchingView) {
    // OnRingBell calls MessageBeep, which doesn't touch the view at
    // all — the dispatcher should hand it off and return true even
    // with a null-Dispatcher mock.
    MockMainWindowView view;
    auto d = GhosttyCallbackDispatcher::Create(view);

    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_RING_BELL));
}

// ----- unknown tag -----

TEST(GhosttyCallbackDispatcherTest, UnknownTagReturnsFalse) {
    // Tag value the dispatcher doesn't know — libghostty would log
    // it as unhandled, which is the correct behaviour. Using a
    // value high enough that no near-term GHOSTTY_ACTION_* enum
    // entry could collide.
    MockMainWindowView view;
    auto d = GhosttyCallbackDispatcher::Create(view);

    EXPECT_FALSE(DispatchTag(*d, static_cast<ghostty_action_tag_e>(9999)));
}
