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

    int toggleBackgroundOpacityCalls = 0;
    void ToggleBackgroundOpacity() override { ++toggleBackgroundOpacityCalls; }

    int undoCalls = 0;
    void Undo() override { ++undoCalls; }

    int redoCalls = 0;
    void Redo() override { ++redoCalls; }

    // ----- surface directory (ISurfaceView) -----
    // One mock pane view. FindSurfaceView records which surface was
    // asked for and returns this view for any non-null surface unless
    // a test sets `surfaceViewFor` to narrow it — that lets tests
    // cover both "routes to the owning pane" and "unknown surface is
    // dropped" without a second mock. Call counters and last-values
    // live on the view; the aliases below keep the field names the
    // existing tests use.
    struct MockSurfaceView : core::host::ISurfaceView {
        int setCursorShapeCalls = 0;
        ghostty_action_mouse_shape_e lastCursorShape{};
        void SetCursorShape(ghostty_action_mouse_shape_e shape) override {
            ++setCursorShapeCalls; lastCursorShape = shape;
        }
        int setMouseVisibilityCalls = 0;
        bool lastMouseVisible = true;
        void SetMouseVisibility(bool visible) override {
            ++setMouseVisibilityCalls; lastMouseVisible = visible;
        }
        int setHoveredLinkCalls = 0;
        std::wstring lastHoveredLinkUrl;
        void SetHoveredLink(std::wstring url) override {
            ++setHoveredLinkCalls; lastHoveredLinkUrl = std::move(url);
        }
        int setSecureInputCalls = 0;
        ghostty_action_secure_input_e lastSecureInputMode{};
        void SetSecureInput(ghostty_action_secure_input_e mode) override {
            ++setSecureInputCalls; lastSecureInputMode = mode;
        }
        int setReadonlyCalls = 0;
        bool lastReadonly = false;
        void SetReadonly(bool readonly) override {
            ++setReadonlyCalls; lastReadonly = readonly;
        }
        int appendKeySequenceCalls = 0;
        std::wstring lastKeySequenceLabel;
        void AppendKeySequence(std::wstring label) override {
            ++appendKeySequenceCalls; lastKeySequenceLabel = std::move(label);
        }
        int clearKeySequenceCalls = 0;
        void ClearKeySequence() override { ++clearKeySequenceCalls; }
        int pushKeyTableCalls = 0;
        std::wstring lastKeyTableName;
        void PushKeyTable(std::wstring name) override {
            ++pushKeyTableCalls; lastKeyTableName = std::move(name);
        }
        int popKeyTableCalls = 0;
        bool lastPopKeyTableAll = false;
        void PopKeyTable(bool all) override {
            ++popKeyTableCalls; lastPopKeyTableAll = all;
        }
        int setScrollbarCalls = 0;
        ghostty_action_scrollbar_s lastScrollbar{};
        void SetScrollbar(ghostty_action_scrollbar_s bar) override {
            ++setScrollbarCalls; lastScrollbar = bar;
        }
        int startSearchCalls = 0;
        std::wstring lastStartSearchNeedle;
        void StartSearch(std::wstring needle) override {
            ++startSearchCalls; lastStartSearchNeedle = std::move(needle);
        }
        int endSearchCalls = 0;
        void EndSearch() override { ++endSearchCalls; }
        int setSearchTotalCalls = 0;
        ptrdiff_t lastSearchTotal = -99;
        void SetSearchTotal(ptrdiff_t total) override {
            ++setSearchTotalCalls; lastSearchTotal = total;
        }
        int setSearchSelectedCalls = 0;
        ptrdiff_t lastSearchSelected = -99;
        void SetSearchSelected(ptrdiff_t selected) override {
            ++setSearchSelectedCalls; lastSearchSelected = selected;
        }
    };
    MockSurfaceView surfaceView;
    // When non-null, only this surface resolves to `surfaceView`;
    // any other surface is "not in this window" (nullptr).
    ghostty_surface_t surfaceViewFor = nullptr;
    int findSurfaceViewCalls = 0;
    ghostty_surface_t lastFindSurfaceViewSurface = nullptr;
    core::host::ISurfaceView* FindSurfaceView(ghostty_surface_t surface) override {
        ++findSurfaceViewCalls;
        lastFindSurfaceViewSurface = surface;
        if (!surface) return nullptr;
        if (surfaceViewFor && surface != surfaceViewFor) return nullptr;
        return &surfaceView;
    }
    // Field aliases so the existing tests keep reading naturally.
    int& setCursorShapeCalls = surfaceView.setCursorShapeCalls;
    ghostty_action_mouse_shape_e& lastCursorShape = surfaceView.lastCursorShape;
    int& setMouseVisibilityCalls = surfaceView.setMouseVisibilityCalls;
    bool& lastMouseVisible = surfaceView.lastMouseVisible;
    int& setHoveredLinkCalls = surfaceView.setHoveredLinkCalls;
    std::wstring& lastHoveredLinkUrl = surfaceView.lastHoveredLinkUrl;
    int& setSecureInputCalls = surfaceView.setSecureInputCalls;
    ghostty_action_secure_input_e& lastSecureInputMode = surfaceView.lastSecureInputMode;
    int& setReadonlyCalls = surfaceView.setReadonlyCalls;
    bool& lastReadonly = surfaceView.lastReadonly;
    int& appendKeySequenceCalls = surfaceView.appendKeySequenceCalls;
    std::wstring& lastKeySequenceLabel = surfaceView.lastKeySequenceLabel;
    int& clearKeySequenceCalls = surfaceView.clearKeySequenceCalls;
    int& pushKeyTableCalls = surfaceView.pushKeyTableCalls;
    std::wstring& lastKeyTableName = surfaceView.lastKeyTableName;
    int& popKeyTableCalls = surfaceView.popKeyTableCalls;
    bool& lastPopKeyTableAll = surfaceView.lastPopKeyTableAll;
    int& setScrollbarCalls = surfaceView.setScrollbarCalls;
    ghostty_action_scrollbar_s& lastScrollbar = surfaceView.lastScrollbar;
    int& startSearchCalls = surfaceView.startSearchCalls;
    std::wstring& lastStartSearchNeedle = surfaceView.lastStartSearchNeedle;
    int& endSearchCalls = surfaceView.endSearchCalls;
    int& setSearchTotalCalls = surfaceView.setSearchTotalCalls;
    ptrdiff_t& lastSearchTotal = surfaceView.lastSearchTotal;
    int& setSearchSelectedCalls = surfaceView.setSearchSelectedCalls;
    ptrdiff_t& lastSearchSelected = surfaceView.lastSearchSelected;
    // The "which surface" the old relays recorded is now the
    // directory's last lookup.
    ghostty_surface_t& lastCursorShapeSurface = lastFindSurfaceViewSurface;
    ghostty_surface_t& lastHoveredLinkSurface = lastFindSurfaceViewSurface;
    ghostty_surface_t& lastSecureInputSurface = lastFindSurfaceViewSurface;
    ghostty_surface_t& lastReadonlySurface = lastFindSurfaceViewSurface;
    ghostty_surface_t& lastKeySequenceSurface = lastFindSurfaceViewSurface;
    ghostty_surface_t& lastKeyTableSurface = lastFindSurfaceViewSurface;
    ghostty_surface_t& lastMouseVisibilitySurface = lastFindSurfaceViewSurface;
    ghostty_surface_t& lastScrollbarSurface = lastFindSurfaceViewSurface;
    ghostty_surface_t& lastStartSearchSurface = lastFindSurfaceViewSurface;
    ghostty_surface_t& lastEndSearchSurface = lastFindSurfaceViewSurface;

    int applyCellSizeCalls = 0;
    ghostty_surface_t lastCellSizeSurface = nullptr;
    ghostty_action_cell_size_s lastCellSize{};
    void ApplyCellSizeForSurface(ghostty_surface_t surface,
                                 ghostty_action_cell_size_s cell) override {
        ++applyCellSizeCalls;
        lastCellSizeSurface = surface;
        lastCellSize = cell;
    }

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

    int promptTitleCalls = 0;
    ghostty_surface_t lastPromptTitleSurface = nullptr;
    void PromptTitleForSurface(ghostty_surface_t s) override {
        ++promptTitleCalls;
        lastPromptTitleSurface = s;
    }

    int notifyCommandFinishedCalls = 0;
    ghostty_surface_t lastCommandFinishedSurface = nullptr;
    int lastCommandExitCode = 0;
    uint64_t lastCommandDurationNs = 0;
    void NotifyCommandFinishedForSurface(ghostty_surface_t s, int exitCode,
                                         uint64_t durationNs) override {
        ++notifyCommandFinishedCalls;
        lastCommandFinishedSurface = s;
        lastCommandExitCode = exitCode;
        lastCommandDurationNs = durationNs;
    }

    int setPwdCalls = 0;
    ghostty_surface_t lastPwdSurface = nullptr;
    std::wstring lastPwd;
    void SetPwdForSurface(ghostty_surface_t s, std::wstring pwd) override {
        ++setPwdCalls;
        lastPwdSurface = s;
        lastPwd = std::move(pwd);
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
