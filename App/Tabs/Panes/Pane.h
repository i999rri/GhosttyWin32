#pragma once

#include "Tabs/Panes/PaneId.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

namespace winrt::GhosttyWin32::implementation {

// A single terminal — the tree's leaf. `content` is UIElement (not
// TerminalControl) so layout-only tests can substitute a placeholder.
struct Pane {
    Microsoft::UI::Xaml::UIElement content{ nullptr };
    PaneId id{};
};

}  // namespace winrt::GhosttyWin32::implementation
