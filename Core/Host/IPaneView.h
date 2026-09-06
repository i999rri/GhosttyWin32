#pragma once

#include "Ghostty/Surface.h"
#include "ghostty.h"
#include <windows.h>
#include <winrt/Windows.UI.h>
#include <functional>

namespace core::host {

// What a pane's owners — the tab, the split panel, the window — need
// from the control living in it, as an interface so the pane tree can
// hold panes without naming the App's TerminalControl (that is what
// lets the tree live in Core and run under test).
//
// Sibling of ISurfaceView, split by who is calling: ISurfaceView is
// libghostty's actions arriving AT the pane; IPaneView is the host
// managing the pane — its lifecycle, its focus, its window-scoped
// looks. TerminalControl implements both. Kept deliberately small:
// anything TerminalControl-specific (IME notifications, composition
// handle, the search box) goes through the App-side ControlOf()
// instead of widening this.
//
// Ownership: implemented by the control the pane's `handle` keeps
// alive; the pointer is borrowed and valid while that handle is.
// Every method runs on the UI thread.
class IPaneView {
public:
    virtual ~IPaneView() = default;

    // The pane's ghostty surface wrapper — the typed calls (Owns,
    // SetOcclusion, SetColorScheme, NeedsConfirmQuit, ...) live on
    // it, so this stays one accessor instead of many relays.
    virtual ghostty::Surface&       Surface() noexcept = 0;
    virtual ghostty::Surface const& Surface() const noexcept = 0;

    // Release the surface, swap chain and composition handle, and
    // unhook from the panel. Idempotent.
    virtual void Detach() = 0;

    // Re-point the pane at another window after a tear-out adopt:
    // new host HWND for the composition surface, new focus callback.
    virtual void Rehost(HWND hostHwnd,
                        std::function<void(ghostty_surface_t)> onFocused) = 0;

    // The tab's active-pane visual: bright when `focused`, dimmed
    // otherwise (the per-tab dim invariant lives on Tab).
    virtual void ApplyFocusVisual(bool focused) = 0;

    // The window's background-opacity mode (#69): show or hide the
    // opaque underlay behind the terminal.
    virtual void SetOpaqueBackground(bool opaque, winrt::Windows::UI::Color bg) = 0;

    // Ask the framework to move keyboard focus into the pane;
    // returns whether it accepted.
    virtual bool TakeFocus() = 0;
};

}  // namespace core::host
