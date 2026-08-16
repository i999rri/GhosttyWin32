#pragma once

#include "Tabs/Panes/PaneId.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

namespace winrt::GhosttyWin32::implementation {

// A single terminal — the industry-standard meaning of "pane" (tmux,
// Windows Terminal, iTerm2, Ghostty upstream). Holds the UI element
// that renders the terminal and the id ghostty uses to route
// close_surface_cb back to this specific pane.
//
// UIElement rather than TerminalControl so layout-only tests can build
// a tree with placeholder Borders and the WinUI Panel side doesn't
// depend on TerminalControl's constructor cost.
struct Pane {
    Microsoft::UI::Xaml::UIElement content{ nullptr };
    // Zero sentinel is accepted so layout-only tests can build panes
    // without an allocator. Production callers thread a real
    // PaneIdAllocator through TabFactory.
    PaneId id{};
};

}  // namespace winrt::GhosttyWin32::implementation
