#pragma once

#include "Tabs/Panes/PaneId.h"
#include "TerminalControl.xaml.h"
#include <winrt/Microsoft.UI.Xaml.h>

namespace winrt::GhosttyWin32::implementation {

// A single terminal — the tree's leaf. One pane hosts exactly one
// TerminalControl, which in turn owns exactly one ghostty surface.
// That 1:1:1 chain is ghostty's own design premise (its split tree
// is SplitTree<SurfaceView>), not a host simplification, so the
// member is typed as the control rather than a generic UIElement.
// Layout code (SplitPanel) still uses `control` as the UIElement it
// is; logic code reaches the implementation through Impl() instead
// of a try_as / get_self pair at every site.
struct Pane {
    winrt::GhosttyWin32::TerminalControl control{ nullptr };
    PaneId id{};

    // The control's implementation object, or nullptr for an empty
    // pane. Borrowed: the projected `control` keeps it alive.
    implementation::TerminalControl* Impl() const noexcept {
        return control
            ? winrt::get_self<implementation::TerminalControl>(control)
            : nullptr;
    }
};

}  // namespace winrt::GhosttyWin32::implementation
