#include "pch.h"
#include "MockMainWindowView.h"
#include "../Core/Ghostty/Actions/Actions.h"

using core::ghostty::actions::Actions;

namespace {

// Fake surface handle. The routing tests never dereference it —
// they only check that the same opaque pointer round-trips through
// GhosttyActions into the view.
ghostty_surface_t FakeSurface(std::uintptr_t v) {
    return reinterpret_cast<ghostty_surface_t>(v);
}

}  // namespace

// ----- view-call routing: stateless dispatchers -----

TEST(GhosttyActionsTest, OnRingBellReturnsTrue) {
    // MessageBeep is fire-and-forget on a Windows system sound;
    // we can't observe the audio side, but the return value is
    // part of the action_cb contract.
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnRingBell());
}

TEST(GhosttyActionsTest, OnRenderTicksTheView) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnRender());
    EXPECT_EQ(view.tickCalls, 1);
}

TEST(GhosttyActionsTest, OnCloseWindowRoutesThroughTryClose) {
    // TryClose is the confirmation-gated close path (issue #102);
    // OnCloseWindow represents user intent, so it goes through
    // TryClose rather than RequestClose to give
    // needs_confirm_quit a chance to prompt.
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnCloseWindow());
    EXPECT_EQ(view.tryCloseCalls, 1);
    EXPECT_EQ(view.requestCloseCalls, 0);
}

TEST(GhosttyActionsTest, OnToggleFullscreenAsksTheViewToToggle) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnToggleFullscreen());
    EXPECT_EQ(view.toggleFullscreenCalls, 1);
}

TEST(GhosttyActionsTest, OnToggleWindowDecorationsAsksTheViewToToggle) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnToggleWindowDecorations());
    EXPECT_EQ(view.toggleWindowDecorationsCalls, 1);
}

TEST(GhosttyActionsTest, OnPresentTerminalAsksTheView) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnPresentTerminal());
    EXPECT_EQ(view.presentTerminalCalls, 1);
}

TEST(GhosttyActionsTest, OnShowOnScreenKeyboardAsksTheView) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnShowOnScreenKeyboard());
    EXPECT_EQ(view.showOnScreenKeyboardCalls, 1);
}

// ----- tab lifecycle / navigation / title -----

TEST(GhosttyActionsTest, OnNewTabCreatesATab) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnNewTab());
    EXPECT_EQ(view.createTabCalls, 1);
}

TEST(GhosttyActionsTest, OnCloseTabForwardsTheSurface) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0x1234);
    EXPECT_TRUE(actions.OnCloseTab(surface));
    EXPECT_EQ(view.closeTabBySurfaceCalls, 1);
    EXPECT_EQ(view.lastCloseTabSurface, surface);
}

TEST(GhosttyActionsTest, OnCloseTabIgnoresNullSurface) {
    // Null surface guard: ghostty shouldn't deliver one, but the
    // contract is "don't dispatch a tear-down for nothing".
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnCloseTab(nullptr));
    EXPECT_EQ(view.closeTabBySurfaceCalls, 0);
}

TEST(GhosttyActionsTest, OnMoveTabForwardsTheAmount) {
    MockMainWindowView view;
    Actions actions(view);
    ghostty_action_move_tab_s move{};
    move.amount = 2;
    EXPECT_TRUE(actions.OnMoveTab(move));
    EXPECT_EQ(view.moveActiveTabByCalls, 1);
    EXPECT_EQ(view.lastMoveTabAmount, 2);
}

TEST(GhosttyActionsTest, OnMoveTabAllowsNegativeAmount) {
    // Negative amount = shift toward lower indices. The action
    // handler itself doesn't clamp — that's the view's job.
    MockMainWindowView view;
    Actions actions(view);
    ghostty_action_move_tab_s move{};
    move.amount = -3;
    EXPECT_TRUE(actions.OnMoveTab(move));
    EXPECT_EQ(view.lastMoveTabAmount, -3);
}

TEST(GhosttyActionsTest, OnGotoTabForwardsTheIndex) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnGotoTab(3));
    EXPECT_EQ(view.goToTabCalls, 1);
    EXPECT_EQ(view.lastGoToTabIndex, 3);
}

TEST(GhosttyActionsTest, OnSetTitleConvertsUtf8ToUtf16) {
    // The view side wants wstring (winrt::hstring is UTF-16 under
    // the hood); GhosttyActions does the conversion on the
    // renderer thread so the captured lambda carries native
    // strings only.
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0x5678);
    EXPECT_TRUE(actions.OnSetTitle(surface, "hello"));
    EXPECT_EQ(view.setTabTitleCalls, 1);
    EXPECT_EQ(view.lastSetTitleSurface, surface);
    EXPECT_EQ(view.lastSetTitleValue, L"hello");
}

TEST(GhosttyActionsTest, OnSetTitleIgnoresEmptyAndNull) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0x42);
    EXPECT_TRUE(actions.OnSetTitle(surface, nullptr));
    EXPECT_TRUE(actions.OnSetTitle(surface, ""));
    EXPECT_TRUE(actions.OnSetTitle(nullptr, "hello"));
    EXPECT_EQ(view.setTabTitleCalls, 0);
}

TEST(GhosttyActionsTest, OnCopyTitleToClipboardForwardsTheSurface) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0x99);
    EXPECT_TRUE(actions.OnCopyTitleToClipboard(surface));
    EXPECT_EQ(view.copyTabTitleCalls, 1);
    EXPECT_EQ(view.lastCopyTitleSurface, surface);
}

// ----- split-pane -----

TEST(GhosttyActionsTest, OnNewSplitForwardsSurfaceAndDirection) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0xAA);
    EXPECT_TRUE(actions.OnNewSplit(surface, GHOSTTY_SPLIT_DIRECTION_RIGHT));
    EXPECT_EQ(view.splitActivePaneCalls, 1);
    EXPECT_EQ(view.lastSplitSurface, surface);
    EXPECT_EQ(view.lastSplitDirection, GHOSTTY_SPLIT_DIRECTION_RIGHT);
}

TEST(GhosttyActionsTest, OnResizeSplitForwardsValue) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0xBB);
    ghostty_action_resize_split_s r{};
    r.amount = 50;
    EXPECT_TRUE(actions.OnResizeSplit(surface, r));
    EXPECT_EQ(view.resizeSplitCalls, 1);
    EXPECT_EQ(view.lastResizeSurface, surface);
    EXPECT_EQ(view.lastResize.amount, 50);
}

TEST(GhosttyActionsTest, OnGotoSplitForwardsDirection) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0xCC);
    EXPECT_TRUE(actions.OnGotoSplit(surface, GHOSTTY_GOTO_SPLIT_NEXT));
    EXPECT_EQ(view.gotoSplitCalls, 1);
    EXPECT_EQ(view.lastGotoSplitSurface, surface);
    EXPECT_EQ(view.lastGotoSplitDirection, GHOSTTY_GOTO_SPLIT_NEXT);
}

TEST(GhosttyActionsTest, OnEqualizeSplitsForwardsSurface) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0xDD);
    EXPECT_TRUE(actions.OnEqualizeSplits(surface));
    EXPECT_EQ(view.equalizeSplitsCalls, 1);
    EXPECT_EQ(view.lastEqualizeSurface, surface);
}

TEST(GhosttyActionsTest, OnToggleSplitZoomForwardsSurface) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0xEE);
    EXPECT_TRUE(actions.OnToggleSplitZoom(surface));
    EXPECT_EQ(view.toggleSplitZoomCalls, 1);
    EXPECT_EQ(view.lastToggleZoomSurface, surface);
}

TEST(GhosttyActionsTest, SplitOperationsIgnoreNullSurface) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnNewSplit(nullptr, GHOSTTY_SPLIT_DIRECTION_RIGHT));
    EXPECT_TRUE(actions.OnResizeSplit(nullptr, {}));
    EXPECT_TRUE(actions.OnGotoSplit(nullptr, GHOSTTY_GOTO_SPLIT_NEXT));
    EXPECT_TRUE(actions.OnEqualizeSplits(nullptr));
    EXPECT_TRUE(actions.OnToggleSplitZoom(nullptr));
    EXPECT_EQ(view.splitActivePaneCalls, 0);
    EXPECT_EQ(view.resizeSplitCalls, 0);
    EXPECT_EQ(view.gotoSplitCalls, 0);
    EXPECT_EQ(view.equalizeSplitsCalls, 0);
    EXPECT_EQ(view.toggleSplitZoomCalls, 0);
}

// ----- window state -----

TEST(GhosttyActionsTest, OnSizeLimitForwardsValue) {
    MockMainWindowView view;
    Actions actions(view);
    ghostty_action_size_limit_s l{};
    l.min_width = 100;
    l.min_height = 50;
    l.max_width = 800;
    l.max_height = 600;
    EXPECT_TRUE(actions.OnSizeLimit(l));
    EXPECT_EQ(view.applySizeLimitCalls, 1);
    EXPECT_EQ(view.lastSizeLimit.min_width, 100u);
    EXPECT_EQ(view.lastSizeLimit.min_height, 50u);
    EXPECT_EQ(view.lastSizeLimit.max_width, 800u);
    EXPECT_EQ(view.lastSizeLimit.max_height, 600u);
}

// ----- terminal-driven appearance -----

TEST(GhosttyActionsTest, OnColorChangeBackgroundRoutesRGB) {
    MockMainWindowView view;
    Actions actions(view);
    ghostty_action_color_change_s cc{};
    cc.kind = GHOSTTY_ACTION_COLOR_KIND_BACKGROUND;
    cc.r = 0x12;
    cc.g = 0x34;
    cc.b = 0x56;
    EXPECT_TRUE(actions.OnColorChange(cc));
    EXPECT_EQ(view.applyBackgroundColorCalls, 1);
    EXPECT_EQ(view.lastBgR, 0x12);
    EXPECT_EQ(view.lastBgG, 0x34);
    EXPECT_EQ(view.lastBgB, 0x56);
}

TEST(GhosttyActionsTest, OnColorChangeNonBackgroundIsIgnored) {
    // Only KIND_BACKGROUND drives our title bar / XAML chrome.
    // FOREGROUND / CURSOR / palette indices belong to the
    // terminal surface; ghostty handles them internally.
    MockMainWindowView view;
    Actions actions(view);
    ghostty_action_color_change_s cc{};
    cc.kind = GHOSTTY_ACTION_COLOR_KIND_CURSOR;
    cc.r = 0xff;
    EXPECT_TRUE(actions.OnColorChange(cc));
    EXPECT_EQ(view.applyBackgroundColorCalls, 0);
}

TEST(GhosttyActionsTest, OnMouseShapeForwardsSurfaceAndShape) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0xBEEF);
    EXPECT_TRUE(actions.OnMouseShape(surface, GHOSTTY_MOUSE_SHAPE_POINTER));
    EXPECT_EQ(view.setCursorShapeCalls, 1);
    EXPECT_EQ(view.lastCursorShapeSurface, surface);
    EXPECT_EQ(view.lastCursorShape, GHOSTTY_MOUSE_SHAPE_POINTER);
}

TEST(GhosttyActionsTest, OnMouseShapeIgnoresNullSurface) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnMouseShape(nullptr, GHOSTTY_MOUSE_SHAPE_TEXT));
    EXPECT_EQ(view.setCursorShapeCalls, 0);
}

TEST(GhosttyActionsTest, OnMouseOverLinkForwardsLengthBoundedUrl) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0xBEEF);
    // Buffer deliberately longer than len: the url field is not
    // NUL-terminated, so len must bound the conversion — reading to
    // the buffer's NUL would leak the trailing bytes into the banner.
    const char buf[] = "https://example.com/aaaaTRAILING";
    ghostty_action_mouse_over_link_s link{ buf, 24 };
    EXPECT_TRUE(actions.OnMouseOverLink(surface, link));
    EXPECT_EQ(view.setHoveredLinkCalls, 1);
    EXPECT_EQ(view.lastHoveredLinkSurface, surface);
    EXPECT_EQ(view.lastHoveredLinkUrl, L"https://example.com/aaaa");
}

TEST(GhosttyActionsTest, OnMouseOverLinkEmptyPayloadClearsTheBanner) {
    // len == 0 is how ghostty says "the pointer left the link" — the
    // empty string must still reach the view so the banner hides.
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0xBEEF);
    ghostty_action_mouse_over_link_s link{ nullptr, 0 };
    EXPECT_TRUE(actions.OnMouseOverLink(surface, link));
    EXPECT_EQ(view.setHoveredLinkCalls, 1);
    EXPECT_TRUE(view.lastHoveredLinkUrl.empty());
}

TEST(GhosttyActionsTest, OnMouseVisibilityMapsEnumToBool) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0xF0);
    EXPECT_TRUE(actions.OnMouseVisibility(surface, GHOSTTY_MOUSE_HIDDEN));
    EXPECT_EQ(view.setMouseVisibilityCalls, 1);
    EXPECT_EQ(view.lastMouseVisibilitySurface, surface);
    EXPECT_FALSE(view.lastMouseVisible);
    EXPECT_TRUE(actions.OnMouseVisibility(surface, GHOSTTY_MOUSE_VISIBLE));
    EXPECT_EQ(view.setMouseVisibilityCalls, 2);
    EXPECT_TRUE(view.lastMouseVisible);
}

TEST(GhosttyActionsTest, OnMouseVisibilityIgnoresNullSurface) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnMouseVisibility(nullptr, GHOSTTY_MOUSE_HIDDEN));
    EXPECT_EQ(view.setMouseVisibilityCalls, 0);
}

TEST(GhosttyActionsTest, OnSecureInputForwardsSurfaceAndMode) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0x5EC);
    EXPECT_TRUE(actions.OnSecureInput(surface, GHOSTTY_SECURE_INPUT_TOGGLE));
    EXPECT_EQ(view.setSecureInputCalls, 1);
    EXPECT_EQ(view.lastSecureInputSurface, surface);
    EXPECT_EQ(view.lastSecureInputMode, GHOSTTY_SECURE_INPUT_TOGGLE);
}

TEST(GhosttyActionsTest, OnSecureInputIgnoresNullSurface) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnSecureInput(nullptr, GHOSTTY_SECURE_INPUT_ON));
    EXPECT_EQ(view.setSecureInputCalls, 0);
}

TEST(GhosttyActionsTest, OnCommandFinishedForwardsExitCodeAndDuration) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0xCF);
    ghostty_action_command_finished_s cf{};
    cf.exit_code = 2;
    cf.duration = 7'000'000'000ull;
    EXPECT_TRUE(actions.OnCommandFinished(surface, cf));
    EXPECT_EQ(view.notifyCommandFinishedCalls, 1);
    EXPECT_EQ(view.lastCommandFinishedSurface, surface);
    EXPECT_EQ(view.lastCommandExitCode, 2);
    EXPECT_EQ(view.lastCommandDurationNs, 7'000'000'000ull);
}

TEST(GhosttyActionsTest, OnCommandFinishedIgnoresNullSurface) {
    MockMainWindowView view;
    Actions actions(view);
    ghostty_action_command_finished_s cf{};
    EXPECT_TRUE(actions.OnCommandFinished(nullptr, cf));
    EXPECT_EQ(view.notifyCommandFinishedCalls, 0);
}

TEST(GhosttyActionsTest, OnPwdForwardsUtf16Path) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0x9D);
    EXPECT_TRUE(actions.OnPwd(surface, "C:/Users/dev/repos"));
    EXPECT_EQ(view.setPwdCalls, 1);
    EXPECT_EQ(view.lastPwdSurface, surface);
    EXPECT_EQ(view.lastPwd, L"C:/Users/dev/repos");
}

TEST(GhosttyActionsTest, OnPwdEmptyOrNullClearsTooltip) {
    // Both reach the view as an empty string — the view clears the
    // tooltip; dropping them would leave a stale path displayed.
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0x9D);
    EXPECT_TRUE(actions.OnPwd(surface, ""));
    EXPECT_TRUE(actions.OnPwd(surface, nullptr));
    EXPECT_EQ(view.setPwdCalls, 2);
    EXPECT_TRUE(view.lastPwd.empty());
}

TEST(GhosttyActionsTest, OnPwdIgnoresNullSurface) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnPwd(nullptr, "C:/x"));
    EXPECT_EQ(view.setPwdCalls, 0);
}

TEST(GhosttyActionsTest, OnKeySequenceAppendsFormattedTrigger) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0x5E9);
    ghostty_action_key_sequence_s seq{};
    seq.active = true;
    seq.trigger.tag = GHOSTTY_TRIGGER_UNICODE;
    seq.trigger.key.unicode = U'a';
    seq.trigger.mods = GHOSTTY_MODS_CTRL;
    EXPECT_TRUE(actions.OnKeySequence(surface, seq));
    EXPECT_EQ(view.appendKeySequenceCalls, 1);
    EXPECT_EQ(view.lastKeySequenceSurface, surface);
    EXPECT_EQ(view.lastKeySequenceLabel, L"ctrl+a");
    EXPECT_EQ(view.clearKeySequenceCalls, 0);
}

TEST(GhosttyActionsTest, OnKeySequenceInactiveClears) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0x5E9);
    ghostty_action_key_sequence_s seq{};
    seq.active = false;
    EXPECT_TRUE(actions.OnKeySequence(surface, seq));
    EXPECT_EQ(view.clearKeySequenceCalls, 1);
    EXPECT_EQ(view.appendKeySequenceCalls, 0);
}

TEST(GhosttyActionsTest, OnKeyTableActivatePushesLengthBoundedName) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0x7AB);
    // Longer buffer than len — the name is not NUL-terminated.
    const char buf[] = "resizeTRAILING";
    ghostty_action_key_table_s kt{};
    kt.tag = GHOSTTY_KEY_TABLE_ACTIVATE;
    kt.value.activate = { buf, 6 };
    EXPECT_TRUE(actions.OnKeyTable(surface, kt));
    EXPECT_EQ(view.pushKeyTableCalls, 1);
    EXPECT_EQ(view.lastKeyTableSurface, surface);
    EXPECT_EQ(view.lastKeyTableName, L"resize");
}

TEST(GhosttyActionsTest, OnKeyTableDeactivateVariantsPop) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0x7AB);
    ghostty_action_key_table_s kt{};
    kt.tag = GHOSTTY_KEY_TABLE_DEACTIVATE;
    EXPECT_TRUE(actions.OnKeyTable(surface, kt));
    EXPECT_EQ(view.popKeyTableCalls, 1);
    EXPECT_FALSE(view.lastPopKeyTableAll);
    kt.tag = GHOSTTY_KEY_TABLE_DEACTIVATE_ALL;
    EXPECT_TRUE(actions.OnKeyTable(surface, kt));
    EXPECT_EQ(view.popKeyTableCalls, 2);
    EXPECT_TRUE(view.lastPopKeyTableAll);
}

TEST(GhosttyActionsTest, KeyStateHandlersIgnoreNullSurface) {
    MockMainWindowView view;
    Actions actions(view);
    ghostty_action_key_sequence_s seq{};
    seq.active = true;
    EXPECT_TRUE(actions.OnKeySequence(nullptr, seq));
    ghostty_action_key_table_s kt{};
    kt.tag = GHOSTTY_KEY_TABLE_DEACTIVATE;
    EXPECT_TRUE(actions.OnKeyTable(nullptr, kt));
    EXPECT_EQ(view.appendKeySequenceCalls, 0);
    EXPECT_EQ(view.popKeyTableCalls, 0);
}

TEST(GhosttyActionsTest, OnMouseOverLinkIgnoresNullSurface) {
    MockMainWindowView view;
    Actions actions(view);
    ghostty_action_mouse_over_link_s link{ "https://x", 9 };
    EXPECT_TRUE(actions.OnMouseOverLink(nullptr, link));
    EXPECT_EQ(view.setHoveredLinkCalls, 0);
}

// ----- config -----

TEST(GhosttyActionsTest, OnReloadConfigPassesSoftFlagThrough) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnReloadConfig(true));
    EXPECT_EQ(view.reloadConfigCalls, 1);
    EXPECT_TRUE(view.lastReloadSoft);

    EXPECT_TRUE(actions.OnReloadConfig(false));
    EXPECT_EQ(view.reloadConfigCalls, 2);
    EXPECT_FALSE(view.lastReloadSoft);
}

TEST(GhosttyActionsTest, OnConfigChangeClonesBeforeHandoff) {
    // CONFIG_CHANGE notification carries a config pointer ghostty
    // owns. GhosttyActions must clone before handing off, so the
    // view can take ownership without freeing libghostty's
    // internal state.
    MockMainWindowView view;
    Actions actions(view);

    // libghostty needs its global init before any *_new — production
    // calls ghostty_init() inside ghostty::App::Create before
    // ghostty_config_new(). Skipping it leaves libghostty's globals
    // (logger, etc.) zeroed and the subsequent clone AVs with
    // SEH 0xc0000005. ghostty_init is idempotent so calling it from
    // the test setup is safe even if a prior test already did.
    ghostty_init(0, nullptr);
    ghostty_config_t cfg = ghostty_config_new();
    ASSERT_NE(cfg, nullptr);

    EXPECT_TRUE(actions.OnConfigChange(cfg));
    EXPECT_EQ(view.replaceConfigCalls, 1);
    ASSERT_NE(view.lastReplacedConfig, nullptr);
    // Different pointer = a clone, not the original.
    EXPECT_NE(view.lastReplacedConfig, cfg);

    ghostty_config_free(cfg);
}

TEST(GhosttyActionsTest, OnConfigChangeIgnoresNull) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnConfigChange(nullptr));
    EXPECT_EQ(view.replaceConfigCalls, 0);
}

// ----- desktop notification -----

TEST(GhosttyActionsTest, OnDesktopNotificationConvertsBothLines) {
    MockMainWindowView view;
    Actions actions(view);
    auto surface = FakeSurface(0xCAFE);
    ghostty_action_desktop_notification_s dn{};
    dn.title = "Title";
    dn.body = "Body";
    EXPECT_TRUE(actions.OnDesktopNotification(surface, dn));
    EXPECT_EQ(view.showDesktopNotificationCalls, 1);
    EXPECT_EQ(view.lastNotificationSurface, surface);
    EXPECT_EQ(view.lastNotificationTitle, L"Title");
    EXPECT_EQ(view.lastNotificationBody, L"Body");
}

TEST(GhosttyActionsTest, OnDesktopNotificationDropsAllEmpty) {
    // Both lines empty → nothing meaningful to show.
    MockMainWindowView view;
    Actions actions(view);
    ghostty_action_desktop_notification_s dn{};
    dn.title = "";
    dn.body = nullptr;
    EXPECT_TRUE(actions.OnDesktopNotification(FakeSurface(0xCAFE), dn));
    EXPECT_EQ(view.showDesktopNotificationCalls, 0);
}

// ----- progress -----

TEST(GhosttyActionsTest, OnProgressReportForwardsStateAndValue) {
    MockMainWindowView view;
    Actions actions(view);
    ghostty_action_progress_report_s pr{};
    pr.state = GHOSTTY_PROGRESS_STATE_SET;
    pr.progress = 42;
    EXPECT_TRUE(actions.OnProgressReport(pr));
    EXPECT_EQ(view.reportProgressCalls, 1);
    EXPECT_EQ(view.lastProgress.state, GHOSTTY_PROGRESS_STATE_SET);
    EXPECT_EQ(view.lastProgress.progress, 42);
}

// ----- state owned by GhosttyActions itself -----

TEST(GhosttyActionsTest, ResetWindowSizeIsSafeBeforeAnyInitialSize) {
    // OnInitialSize records the width/height; OnResetWindowSize
    // reads it back. With no prior InitialSize and a null HWND
    // from the mock, ResetWindowSize must still ack without
    // crashing — the dispatch lambda just no-ops at the HWND
    // check.
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnResetWindowSize());
}

TEST(GhosttyActionsTest, OnInitialSizeFollowedByResetIsSafe) {
    MockMainWindowView view;
    Actions actions(view);
    ghostty_action_initial_size_s sz{};
    sz.width = 1024;
    sz.height = 768;
    EXPECT_TRUE(actions.OnInitialSize(sz));
    // No view-side observable yet (the bounce into SetWindowPos
    // happens directly via Hwnd(), which the mock returns null
    // for); the cross-handler state link is exercised in
    // integration tests where a real window exists.
    EXPECT_TRUE(actions.OnResetWindowSize());
}

// ----- Detach: dispatched work must not reach a closing view -----

TEST(GhosttyActionsTest, DispatchedWorkReachesTheViewWhileAttached) {
    MockMainWindowView view;
    Actions actions(view);
    EXPECT_TRUE(actions.OnMouseShape(FakeSurface(0x10),
                                     GHOSTTY_MOUSE_SHAPE_TEXT));
    EXPECT_EQ(view.setCursorShapeCalls, 1);
}

TEST(GhosttyActionsTest, DetachDisarmsDispatchedWork) {
    // The mock runs Dispatch inline, so this models the issue #131
    // ordering: work enqueued before the window died would run after
    // it. With the liveness gate cleared, the lambda must no-op
    // instead of touching the view.
    MockMainWindowView view;
    Actions actions(view);
    actions.Detach();
    EXPECT_TRUE(actions.OnMouseShape(FakeSurface(0x10),
                                     GHOSTTY_MOUSE_SHAPE_TEXT));
    EXPECT_EQ(view.setCursorShapeCalls, 0);
}

TEST(GhosttyActionsTest, DetachIsIdempotent) {
    MockMainWindowView view;
    Actions actions(view);
    actions.Detach();
    actions.Detach();
    EXPECT_TRUE(actions.OnMouseShape(FakeSurface(0x10),
                                     GHOSTTY_MOUSE_SHAPE_TEXT));
    EXPECT_EQ(view.setCursorShapeCalls, 0);
}
