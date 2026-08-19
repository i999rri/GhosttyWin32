#pragma once

#include "../Core/Host/IWindow.h"
#include <cstdint>
#include <string>

// Capturing IMainWindowView fake for GhosttyActions / dispatcher
// tests. Each overridden method records how many times it was
// called and the last arguments it saw, so tests can assert
// against simple integer / value comparisons rather than wiring
// up a heavier mock framework.
//
// Dispatch runs the queued function inline — production
// translates this to DispatcherQueue::TryEnqueue, but the test
// only cares that the queued work eventually runs and
// "immediately" is the simplest schedule that gives synchronous
// observability.
struct MockMainWindowView : core::host::IWindow {
    // ----- base view methods -----
    int dispatchCalls = 0;
    int tickCalls = 0;
    int requestCloseCalls = 0;
    int tryCloseCalls = 0;

    HWND Hwnd() const noexcept override { return nullptr; }
    void Dispatch(std::function<void()> fn) override {
        ++dispatchCalls;
        if (fn) fn();
    }
    void Tick() override { ++tickCalls; }
    void RequestClose() override { ++requestCloseCalls; }
    void TryClose() override { ++tryCloseCalls; }

    // ----- split-pane -----
    int splitActivePaneCalls = 0;
    ghostty_surface_t lastSplitSurface = nullptr;
    ghostty_action_split_direction_e lastSplitDirection{};
    void SplitActivePane(ghostty_surface_t s,
                         ghostty_action_split_direction_e d) override {
        ++splitActivePaneCalls;
        lastSplitSurface = s;
        lastSplitDirection = d;
    }

    int resizeSplitCalls = 0;
    ghostty_surface_t lastResizeSurface = nullptr;
    ghostty_action_resize_split_s lastResize{};
    void ResizeSplitFromAction(ghostty_surface_t s,
                               ghostty_action_resize_split_s r) override {
        ++resizeSplitCalls;
        lastResizeSurface = s;
        lastResize = r;
    }

    int gotoSplitCalls = 0;
    ghostty_surface_t lastGotoSplitSurface = nullptr;
    ghostty_action_goto_split_e lastGotoSplitDirection{};
    void GotoSplitFromAction(ghostty_surface_t s,
                             ghostty_action_goto_split_e d) override {
        ++gotoSplitCalls;
        lastGotoSplitSurface = s;
        lastGotoSplitDirection = d;
    }

    int equalizeSplitsCalls = 0;
    ghostty_surface_t lastEqualizeSurface = nullptr;
    void EqualizeSplitsForSurface(ghostty_surface_t s) override {
        ++equalizeSplitsCalls;
        lastEqualizeSurface = s;
    }

    int toggleSplitZoomCalls = 0;
    ghostty_surface_t lastToggleZoomSurface = nullptr;
    void ToggleSplitZoomForSurface(ghostty_surface_t s) override {
        ++toggleSplitZoomCalls;
        lastToggleZoomSurface = s;
    }

    // ----- tab lifecycle / title -----
    int createTabCalls = 0;
    void CreateTab() override { ++createTabCalls; }

    int closeTabBySurfaceCalls = 0;
    ghostty_surface_t lastCloseTabSurface = nullptr;
    void CloseTabBySurface(ghostty_surface_t s) override {
        ++closeTabBySurfaceCalls;
        lastCloseTabSurface = s;
    }

    int goToTabCalls = 0;
    int lastGoToTabIndex = 0;
    void GoToTab(int i) override {
        ++goToTabCalls;
        lastGoToTabIndex = i;
    }

    int setTabTitleCalls = 0;
    ghostty_surface_t lastSetTitleSurface = nullptr;
    std::wstring lastSetTitleValue;
    void SetTabTitleForSurface(ghostty_surface_t s, std::wstring t) override {
        ++setTabTitleCalls;
        lastSetTitleSurface = s;
        lastSetTitleValue = std::move(t);
    }

    int copyTabTitleCalls = 0;
    ghostty_surface_t lastCopyTitleSurface = nullptr;
    void CopyTabTitleForSurface(ghostty_surface_t s) override {
        ++copyTabTitleCalls;
        lastCopyTitleSurface = s;
    }

    int moveActiveTabByCalls = 0;
    ssize_t lastMoveTabAmount = 0;
    void MoveActiveTabBy(ssize_t amount) override {
        ++moveActiveTabByCalls;
        lastMoveTabAmount = amount;
    }

    // ----- window state -----
    int applySizeLimitCalls = 0;
    ghostty_action_size_limit_s lastSizeLimit{};
    void ApplySizeLimit(ghostty_action_size_limit_s l) override {
        ++applySizeLimitCalls;
        lastSizeLimit = l;
    }

    int toggleFullscreenCalls = 0;
    void ToggleFullscreen() override { ++toggleFullscreenCalls; }

    int toggleWindowDecorationsCalls = 0;
    void ToggleWindowDecorations() override { ++toggleWindowDecorationsCalls; }

    int setFloatOnTopCalls = 0;
    ghostty_action_float_window_e lastFloatOnTopMode{};
    void SetFloatOnTop(ghostty_action_float_window_e mode) override {
        ++setFloatOnTopCalls;
        lastFloatOnTopMode = mode;
    }

    int presentTerminalCalls = 0;
    void PresentTerminal() override { ++presentTerminalCalls; }

    int showOnScreenKeyboardCalls = 0;
    void ShowOnScreenKeyboard() override { ++showOnScreenKeyboardCalls; }

    // ----- terminal-driven appearance + lifecycle -----
    int applyBackgroundColorCalls = 0;
    uint8_t lastBgR = 0, lastBgG = 0, lastBgB = 0;
    void ApplyBackgroundColor(uint8_t r, uint8_t g, uint8_t b) override {
        ++applyBackgroundColorCalls;
        lastBgR = r;
        lastBgG = g;
        lastBgB = b;
    }

    int setCursorShapeCalls = 0;
    ghostty_surface_t lastCursorShapeSurface = nullptr;
    ghostty_action_mouse_shape_e lastCursorShape{};
    void SetCursorShapeForSurface(ghostty_surface_t s,
                                  ghostty_action_mouse_shape_e shape) override {
        ++setCursorShapeCalls;
        lastCursorShapeSurface = s;
        lastCursorShape = shape;
    }

    int setHoveredLinkCalls = 0;
    ghostty_surface_t lastHoveredLinkSurface = nullptr;
    std::wstring lastHoveredLinkUrl;
    void SetHoveredLinkForSurface(ghostty_surface_t s, std::wstring url) override {
        ++setHoveredLinkCalls;
        lastHoveredLinkSurface = s;
        lastHoveredLinkUrl = std::move(url);
    }

    int setMouseVisibilityCalls = 0;
    ghostty_surface_t lastMouseVisibilitySurface = nullptr;
    bool lastMouseVisible = true;
    void SetMouseVisibilityForSurface(ghostty_surface_t s, bool visible) override {
        ++setMouseVisibilityCalls;
        lastMouseVisibilitySurface = s;
        lastMouseVisible = visible;
    }

    int replaceConfigCalls = 0;
    // Records whether the clone passed in differed from the
    // original. The production view takes ownership; the mock
    // frees so the test process exits clean.
    ghostty_config_t lastReplacedConfig = nullptr;
    void ReplaceConfig(ghostty_config_t cloned) override {
        ++replaceConfigCalls;
        lastReplacedConfig = cloned;
        if (cloned) ghostty_config_free(cloned);
    }

    int reloadConfigCalls = 0;
    bool lastReloadSoft = false;
    void ReloadConfig(bool soft) override {
        ++reloadConfigCalls;
        lastReloadSoft = soft;
    }

    int showDesktopNotificationCalls = 0;
    ghostty_surface_t lastNotificationSurface = nullptr;
    std::wstring lastNotificationTitle;
    std::wstring lastNotificationBody;
    void ShowDesktopNotification(ghostty_surface_t surface,
                                 std::wstring title, std::wstring body) override {
        ++showDesktopNotificationCalls;
        lastNotificationSurface = surface;
        lastNotificationTitle = std::move(title);
        lastNotificationBody = std::move(body);
    }

    int reportProgressCalls = 0;
    ghostty_action_progress_report_s lastProgress{};
    void ReportProgress(ghostty_action_progress_report_s pr) override {
        ++reportProgressCalls;
        lastProgress = pr;
    }
};
