#pragma once

#include "../GhosttyWin32App/IMainWindowView.h"

// Minimal IMainWindowView for tests. Every method is a no-op
// (and Dispatcher() returns a null DispatcherQueue) so the test
// surface is "the dispatcher routes the action without touching
// the view". Tests that need to observe a specific view call
// don't fit here — Dispatcher() returning null means any
// TryEnqueue path will fail loudly rather than silently swallow
// the call.
struct MockMainWindowView : winrt::GhosttyWin32::implementation::IMainWindowView {
    HWND Hwnd() const noexcept override { return nullptr; }
    winrt::Microsoft::UI::Dispatching::DispatcherQueue Dispatcher() const override {
        return nullptr;
    }
    void Tick() override {}
    void RequestClose() override {}

    void SplitActivePane(ghostty_surface_t, ghostty_action_split_direction_e) override {}
    void ResizeSplitFromAction(ghostty_surface_t, ghostty_action_resize_split_s) override {}
    void GotoSplitFromAction(ghostty_surface_t, ghostty_action_goto_split_e) override {}
    void EqualizeSplitsForSurface(ghostty_surface_t) override {}
    void ToggleSplitZoomForSurface(ghostty_surface_t) override {}

    void CreateTab() override {}
    void CloseTabBySurface(ghostty_surface_t) override {}
    void GoToTab(int) override {}
    void SetTabTitleForSurface(ghostty_surface_t, std::wstring) override {}
    void CopyTabTitleForSurface(ghostty_surface_t) override {}

    void ApplySizeLimit(ghostty_action_size_limit_s) override {}
    void ToggleFullscreen() override {}

    void ApplyBackgroundColor(uint8_t, uint8_t, uint8_t) override {}
    void SetCursorShapeForSurface(ghostty_surface_t, ghostty_action_mouse_shape_e) override {}
    void ReplaceConfig(ghostty_config_t cloned) override {
        // Avoid leaking the clone GhosttyActions::OnConfigChange
        // produces — the production view takes ownership; the
        // mock just frees so the test process exits clean.
        if (cloned) ghostty_config_free(cloned);
    }
    void ReloadConfig(bool) override {}
    void ShowDesktopNotification(std::wstring, std::wstring) override {}
    void ReportProgress(ghostty_action_progress_report_s) override {}
};
