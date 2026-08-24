#include "pch.h"
#include "MockMainWindowView.h"
#include "../Core/Ghostty/CallbackDispatcher.h"

using core::ghostty::CallbackDispatcher;

namespace {

// Helper: build an empty target/action pair, set the tag, dispatch.
// The ack-only routing doesn't read target.target or action.action,
// so leaving them default-zeroed is safe.
bool DispatchTag(CallbackDispatcher& d, ghostty_action_tag_e tag) {
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
    auto d = CallbackDispatcher::Create(view);

    // READONLY with an app target: upstream logs and ignores —
    // acked. Surface-targeted delivery routes (routing tests below).
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_READONLY));
    EXPECT_EQ(view.setReadonlyCalls, 0);
    // SECURE_INPUT with an app target: macOS's EnableSecureEventInput
    // concept, no Windows counterpart — deliberately acked. The
    // surface-targeted variant routes (see the routing tests below).
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_SECURE_INPUT));
    EXPECT_EQ(view.setSecureInputCalls, 0);
    // KEY_SEQUENCE / KEY_TABLE with an app target: upstream logs a
    // warning and ignores them — acked here. Surface-targeted
    // delivery routes (see the routing tests below).
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_KEY_SEQUENCE));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_KEY_TABLE));
    EXPECT_EQ(view.appendKeySequenceCalls + view.clearKeySequenceCalls +
              view.pushKeyTableCalls + view.popKeyTableCalls, 0);
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_PROMPT_TITLE));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_PWD));
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_COMMAND_FINISHED));
}

TEST(GhosttyCallbackDispatcherTest, FeatureSurfaceAcksReturnTrue) {
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);

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
    auto d = CallbackDispatcher::Create(view);

    // SCROLLBAR: no scrollbar UI to feed.
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_SCROLLBAR));
    // QUIT_TIMER: macOS-only quit countdown.
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_QUIT_TIMER));
    // CELL_SIZE: cached but no host-side consumer yet.
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_CELL_SIZE));
}

TEST(GhosttyCallbackDispatcherTest, UndoRedoRouteToTheView) {
    // Formerly feature-surface acks; #151 wires them to the view's
    // parked-close stack. Empty-stack handling is view state, so
    // the dispatcher's contract is just delivery.
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_UNDO));
    EXPECT_EQ(view.undoCalls, 1);
    EXPECT_EQ(view.redoCalls, 0);
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_REDO));
    EXPECT_EQ(view.redoCalls, 1);
}

TEST(GhosttyCallbackDispatcherTest, ToggleBackgroundOpacityRoutesToTheView) {
    // Was the last "disabled by bug" ack (#69); now routed. The
    // opacity/fullscreen guards are view-side, so the dispatcher's
    // contract is just delivery.
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_TOGGLE_BACKGROUND_OPACITY));
    EXPECT_EQ(view.toggleBackgroundOpacityCalls, 1);
}

TEST(GhosttyCallbackDispatcherTest, FloatWindowRoutesToTheView) {
    // Was part of the "disabled features" ack set; now routed for
    // real (#109). Kept here so the tag can't silently fall back to
    // the default branch.
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_FLOAT_WINDOW));
    EXPECT_EQ(view.setFloatOnTopCalls, 1);
}

// ----- surface-targeted routing -----

TEST(GhosttyCallbackDispatcherTest, MouseOverLinkRoutesToTheOwningSurface) {
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);

    const char url[] = "https://example.com/x";
    ghostty_target_s target{};
    target.tag = GHOSTTY_TARGET_SURFACE;
    target.target.surface = reinterpret_cast<ghostty_surface_t>(0xBEEF);
    ghostty_action_s action{};
    action.tag = GHOSTTY_ACTION_MOUSE_OVER_LINK;
    action.action.mouse_over_link = { url, sizeof(url) - 1 };

    EXPECT_TRUE(d->DispatchAction(target, action));
    EXPECT_EQ(view.setHoveredLinkCalls, 1);
    EXPECT_EQ(view.lastHoveredLinkSurface, target.target.surface);
    EXPECT_EQ(view.lastHoveredLinkUrl, L"https://example.com/x");

    // App-targeted MOUSE_OVER_LINK has no owning pane — refuse
    // (return false) rather than silently ack.
    EXPECT_FALSE(DispatchTag(*d, GHOSTTY_ACTION_MOUSE_OVER_LINK));
}

TEST(GhosttyCallbackDispatcherTest, MouseVisibilityRoutesToTheOwningSurface) {
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);

    ghostty_target_s target{};
    target.tag = GHOSTTY_TARGET_SURFACE;
    target.target.surface = reinterpret_cast<ghostty_surface_t>(0xBEEF);
    ghostty_action_s action{};
    action.tag = GHOSTTY_ACTION_MOUSE_VISIBILITY;
    action.action.mouse_visibility = GHOSTTY_MOUSE_HIDDEN;

    EXPECT_TRUE(d->DispatchAction(target, action));
    EXPECT_EQ(view.setMouseVisibilityCalls, 1);
    EXPECT_EQ(view.lastMouseVisibilitySurface, target.target.surface);
    EXPECT_FALSE(view.lastMouseVisible);

    // App-targeted delivery has no owning pane — refuse.
    EXPECT_FALSE(DispatchTag(*d, GHOSTTY_ACTION_MOUSE_VISIBILITY));
}

TEST(GhosttyCallbackDispatcherTest, PromptTitleRoutesToTheOwningSurface) {
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);

    ghostty_target_s target{};
    target.tag = GHOSTTY_TARGET_SURFACE;
    target.target.surface = reinterpret_cast<ghostty_surface_t>(0xBEEF);
    ghostty_action_s action{};
    action.tag = GHOSTTY_ACTION_PROMPT_TITLE;

    // Both payload variants land on the same handler — one title
    // surface per tab (same collapse as SET_TITLE/SET_TAB_TITLE).
    action.action.prompt_title = GHOSTTY_PROMPT_TITLE_SURFACE;
    EXPECT_TRUE(d->DispatchAction(target, action));
    action.action.prompt_title = GHOSTTY_PROMPT_TITLE_TAB;
    EXPECT_TRUE(d->DispatchAction(target, action));
    EXPECT_EQ(view.promptTitleCalls, 2);
    EXPECT_EQ(view.lastPromptTitleSurface, target.target.surface);

    // App-targeted: upstream logs and ignores — acked.
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_PROMPT_TITLE));
    EXPECT_EQ(view.promptTitleCalls, 2);
}

TEST(GhosttyCallbackDispatcherTest, ReadonlyRoutesToTheOwningSurface) {
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);

    ghostty_target_s target{};
    target.tag = GHOSTTY_TARGET_SURFACE;
    target.target.surface = reinterpret_cast<ghostty_surface_t>(0xBEEF);
    ghostty_action_s action{};
    action.tag = GHOSTTY_ACTION_READONLY;
    action.action.readonly = GHOSTTY_READONLY_ON;

    EXPECT_TRUE(d->DispatchAction(target, action));
    EXPECT_EQ(view.setReadonlyCalls, 1);
    EXPECT_EQ(view.lastReadonlySurface, target.target.surface);
    EXPECT_TRUE(view.lastReadonly);
}

TEST(GhosttyCallbackDispatcherTest, CommandFinishedRoutesToTheOwningSurface) {
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);

    ghostty_target_s target{};
    target.tag = GHOSTTY_TARGET_SURFACE;
    target.target.surface = reinterpret_cast<ghostty_surface_t>(0xBEEF);
    ghostty_action_s action{};
    action.tag = GHOSTTY_ACTION_COMMAND_FINISHED;
    action.action.command_finished = { /*exit_code=*/1,
                                       /*duration=*/6'000'000'000ull };

    EXPECT_TRUE(d->DispatchAction(target, action));
    EXPECT_EQ(view.notifyCommandFinishedCalls, 1);
    EXPECT_EQ(view.lastCommandExitCode, 1);

    // App-targeted: upstream logs and ignores — acked.
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_COMMAND_FINISHED));
    EXPECT_EQ(view.notifyCommandFinishedCalls, 1);
}

TEST(GhosttyCallbackDispatcherTest, PwdRoutesToTheOwningSurface) {
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);

    ghostty_target_s target{};
    target.tag = GHOSTTY_TARGET_SURFACE;
    target.target.surface = reinterpret_cast<ghostty_surface_t>(0xBEEF);
    ghostty_action_s action{};
    action.tag = GHOSTTY_ACTION_PWD;
    action.action.pwd.pwd = "C:/Users/dev";

    EXPECT_TRUE(d->DispatchAction(target, action));
    EXPECT_EQ(view.setPwdCalls, 1);
    EXPECT_EQ(view.lastPwd, L"C:/Users/dev");

    // App-targeted PWD: upstream logs and ignores — acked.
    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_PWD));
    EXPECT_EQ(view.setPwdCalls, 1);
}

TEST(GhosttyCallbackDispatcherTest, KeyStateActionsRouteToTheOwningSurface) {
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);

    ghostty_target_s target{};
    target.tag = GHOSTTY_TARGET_SURFACE;
    target.target.surface = reinterpret_cast<ghostty_surface_t>(0xBEEF);

    ghostty_action_s action{};
    action.tag = GHOSTTY_ACTION_KEY_SEQUENCE;
    action.action.key_sequence.active = true;
    action.action.key_sequence.trigger.tag = GHOSTTY_TRIGGER_UNICODE;
    action.action.key_sequence.trigger.key.unicode = U'a';
    action.action.key_sequence.trigger.mods = GHOSTTY_MODS_CTRL;
    EXPECT_TRUE(d->DispatchAction(target, action));
    EXPECT_EQ(view.appendKeySequenceCalls, 1);
    EXPECT_EQ(view.lastKeySequenceLabel, L"ctrl+a");

    ghostty_action_s tableAction{};
    tableAction.tag = GHOSTTY_ACTION_KEY_TABLE;
    tableAction.action.key_table.tag = GHOSTTY_KEY_TABLE_DEACTIVATE_ALL;
    EXPECT_TRUE(d->DispatchAction(target, tableAction));
    EXPECT_EQ(view.popKeyTableCalls, 1);
    EXPECT_TRUE(view.lastPopKeyTableAll);
}

TEST(GhosttyCallbackDispatcherTest, SecureInputRoutesToTheOwningSurface) {
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);

    ghostty_target_s target{};
    target.tag = GHOSTTY_TARGET_SURFACE;
    target.target.surface = reinterpret_cast<ghostty_surface_t>(0xBEEF);
    ghostty_action_s action{};
    action.tag = GHOSTTY_ACTION_SECURE_INPUT;
    action.action.secure_input = GHOSTTY_SECURE_INPUT_ON;

    EXPECT_TRUE(d->DispatchAction(target, action));
    EXPECT_EQ(view.setSecureInputCalls, 1);
    EXPECT_EQ(view.lastSecureInputSurface, target.target.surface);
    EXPECT_EQ(view.lastSecureInputMode, GHOSTTY_SECURE_INPUT_ON);
}

// ----- view-free live handlers -----

TEST(GhosttyCallbackDispatcherTest, RingBellDispatchesWithoutTouchingView) {
    // OnRingBell calls MessageBeep, which doesn't touch the view at
    // all — the dispatcher should hand it off and return true even
    // with a null-Dispatcher mock.
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);

    EXPECT_TRUE(DispatchTag(*d, GHOSTTY_ACTION_RING_BELL));
}

// ----- unknown tag -----

TEST(GhosttyCallbackDispatcherTest, UnknownTagReturnsFalse) {
    // Tag value the dispatcher doesn't know — libghostty would log
    // it as unhandled, which is the correct behaviour. Using a
    // value high enough that no near-term GHOSTTY_ACTION_* enum
    // entry could collide.
    MockMainWindowView view;
    auto d = CallbackDispatcher::Create(view);

    EXPECT_FALSE(DispatchTag(*d, static_cast<ghostty_action_tag_e>(9999)));
}
