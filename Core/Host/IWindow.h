#pragma once

#include "ghostty.h"
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace core::host {

// Narrow view-side surface that GhosttyCallbackDispatcher (and
// the GhosttyActions handlers it routes to) depends on. Lets the
// dispatcher reach the bits of MainWindow it actually needs
// (HWND, UI dispatcher, ghostty tick) without taking a hard
// dependency on the full MainWindow type — keeps the dispatcher
// testable in isolation and stops it from quietly accreting more
// MainWindow internals than it really needs.
//
// Grown lazily on purpose: every handler that's lifted out of
// MainWindow::action_cb adds the one or two methods here that
// it actually uses. The narrow surface is the design value; a
// full-MainWindow facade would defeat the point of the split.
struct IWindow {
    virtual ~IWindow() = default;

    // Top-level HWND for the host window. Borrowed; valid for the
    // lifetime of the view. Used for ShellExecuteW parenting and
    // any Win32 message-pump round-trip a handler needs.
    virtual HWND Hwnd() const noexcept = 0;

    // Queue a function for execution on the UI thread. action_cb
    // fires from ghostty's renderer thread, so any WinUI / DComp
    // mutation has to bounce through here before touching the
    // visual tree.
    //
    // Returning std::function (rather than the underlying WinUI
    // DispatcherQueue) keeps the interface winrt-free so test
    // doubles don't need a WinAppSDK runtime — see the production
    // override on MainWindow for the WinUI translation.
    virtual void Dispatch(std::function<void()> fn) = 0;

    // Forwarder to ghostty::App::Tick. The underlying ghostty_app_tick
    // call is wrapped with the SEH guard / NVIDIA presenter workaround
    // from issue #26, so handlers should not be tempted to invoke
    // ghostty_app_tick directly.
    virtual void Tick() = 0;

    // Tear the window down. Named "Request" because the WinUI
    // Window::Close already takes that slot in the projection and
    // can't be marked override; this thin wrapper swallows the
    // hresult_error WinUI throws when the window is already torn
    // down, so handlers can call it unconditionally.
    virtual void RequestClose() = 0;

    // ----- split-pane operations -----
    // Surface-target split actions all share the same shape: find
    // the pane owning `surface` in its tab, mutate the tree, leave
    // the result focused. The view owns the tree, so each operation
    // lives there and the dispatcher just forwards the request.
    // UI thread only — callers must already be there.

    // Insert a new pane next to the source pane along `direction`.
    virtual void SplitActivePane(ghostty_surface_t surface,
                                 ghostty_action_split_direction_e direction) = 0;

    // Nudge the nearest ancestor split whose axis matches the
    // direction by `amount` DIPs.
    virtual void ResizeSplitFromAction(ghostty_surface_t surface,
                                       ghostty_action_resize_split_s resize) = 0;

    // Move keyboard focus to another pane. PREVIOUS/NEXT cycle the
    // tree depth-first; directional variants pick the adjacent leaf
    // by arranged-rect adjacency.
    virtual void GotoSplitFromAction(ghostty_surface_t surface,
                                     ghostty_action_goto_split_e direction) = 0;

    // Reset every split ratio in the source surface's tab to 0.5.
    virtual void EqualizeSplitsForSurface(ghostty_surface_t surface) = 0;

    // Expand the source leaf to fill its tab; a second call restores
    // the regular split layout.
    virtual void ToggleSplitZoomForSurface(ghostty_surface_t surface) = 0;

    // ----- tab lifecycle / navigation / title -----
    // UI thread only.

    // Create a new tab and select it. Used by both NEW_TAB and
    // NEW_WINDOW (the single-window build collapses NEW_WINDOW to
    // NEW_TAB; multi-window #55 will give them distinct entries).
    virtual void CreateTab() = 0;

    // Close the tab containing `surface`. Mirrors the
    // TabCloseRequested path (detach every leaf -> RemoveAt ->
    // Close the window when the last tab goes).
    virtual void CloseTabBySurface(ghostty_surface_t surface) = 0;

    // Select a tab by ghostty's goto_tab encoding: PREVIOUS / NEXT /
    // LAST live as negative sentinels, non-negative values are
    // direct indices. Out-of-range indices are ignored.
    virtual void GoToTab(int requested) = 0;

    // Set the header on the tab containing `surface`. Used by both
    // SET_TITLE and SET_TAB_TITLE (this port collapses them; macOS
    // ghostty keeps them distinct because the macOS window title
    // and the in-window tab title aren't the same surface).
    virtual void SetTabTitleForSurface(ghostty_surface_t surface,
                                       std::wstring title) = 0;

    // Copy the header of the tab containing `surface` to the
    // system clipboard. No-op when the tab has no title yet.
    virtual void CopyTabTitleForSurface(ghostty_surface_t surface) = 0;

    // Move the currently-selected tab by `amount` positions (signed).
    // Positive shifts toward higher indices, negative toward lower.
    // The value is clamped to the TabView's bounds; out-of-range
    // values are a no-op rather than a wrap-around — matches the
    // upstream MoveTab semantics.
    virtual void MoveActiveTabBy(ssize_t amount) = 0;

    // ----- window state helpers (delegated to dedicated state
    // owners on MainWindow's side) -----

    // Update the SIZE_LIMIT constraint. The first call also
    // installs the WM_GETMINMAXINFO subclass; subsequent calls
    // just refresh the stored limit.
    virtual void ApplySizeLimit(ghostty_action_size_limit_s limit) = 0;

    // Toggle borderless fullscreen on/off. Restores the exact
    // pre-fullscreen placement + style when leaving.
    virtual void ToggleFullscreen() = 0;

    // Bring the window to the foreground (restoring it from a
    // minimized state first) and give it focus. Used by
    // PRESENT_TERMINAL — typically triggered by an external
    // notification "click me" path.
    virtual void PresentTerminal() = 0;

    // Show the Windows on-screen keyboard so a touch / pen user can
    // type without a physical keyboard. The OSK is its own top-level
    // window; we don't track it and the user dismisses it normally.
    virtual void ShowOnScreenKeyboard() = 0;

    // ----- terminal-driven appearance + lifecycle -----

    // Apply a new background colour (caption bar + XAML root) in
    // response to COLOR_CHANGE / DECCC. R/G/B are 0-255.
    virtual void ApplyBackgroundColor(uint8_t r, uint8_t g, uint8_t b) = 0;

    // Set the pointer cursor shape on the pane owning `surface`.
    // (Currently routes to the active leaf of the tab — issue #65
    // tracks moving this to the originating leaf.)
    virtual void SetCursorShapeForSurface(ghostty_surface_t surface,
                                          ghostty_action_mouse_shape_e shape) = 0;

    // Replace the GhosttyApp's stored config with `cloned`. Takes
    // ownership: the implementation is responsible for freeing it
    // when superseded. Pass a clone from CONFIG_CHANGE (ghostty
    // owns the original) — RELOAD_CONFIG goes through ReloadConfig
    // below instead.
    virtual void ReplaceConfig(ghostty_config_t cloned) = 0;

    // Reload the user config. soft=true re-applies the currently
    // stored config; soft=false re-parses from disk on a 4MB-stack
    // worker thread (ghostty's parser blows the default 1MB
    // stack) and swaps the result in on the UI thread.
    virtual void ReloadConfig(bool soft) = 0;

    // Show a Windows toast for the given two-line payload. Either
    // string may be empty; if both are empty the call is a no-op.
    virtual void ShowDesktopNotification(std::wstring title,
                                         std::wstring body) = 0;

    // Update the host window's taskbar progress indicator. The
    // taskbar surface is window-global, not per-pane, so no
    // surface argument is needed.
    virtual void ReportProgress(ghostty_action_progress_report_s pr) = 0;
};

}  // namespace core::host
