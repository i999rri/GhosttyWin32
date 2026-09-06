#pragma once

#include <Panes/PaneId.h>

namespace winrt::GhosttyWin32::implementation {

// The model type lives in Core (Core/Panes/PaneId.h) so the pane
// tree and its tests can use it without this project; the alias
// keeps every existing spelling in App working.
using PaneId = core::panes::PaneId;

}  // namespace winrt::GhosttyWin32::implementation
