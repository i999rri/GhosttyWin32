#pragma once

#include "ghostty.h"
#include <winrt/Microsoft.UI.Dispatching.h>
#include <windows.h>

namespace winrt::GhosttyWin32::implementation {

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
struct IMainWindowView {
    virtual ~IMainWindowView() = default;

    // Top-level HWND for the host window. Borrowed; valid for the
    // lifetime of the view. Used for ShellExecuteW parenting and
    // any Win32 message-pump round-trip a handler needs.
    virtual HWND Hwnd() const noexcept = 0;

    // UI thread DispatcherQueue. action_cb fires from ghostty's
    // renderer thread, so any WinUI / DComp mutation has to bounce
    // through here before touching the visual tree.
    virtual winrt::Microsoft::UI::Dispatching::DispatcherQueue Dispatcher() const = 0;

    // Forwarder to GhosttyApp::Tick. The underlying ghostty_app_tick
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
};

}  // namespace winrt::GhosttyWin32::implementation
