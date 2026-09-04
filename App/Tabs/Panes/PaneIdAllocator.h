#pragma once

#include <Panes/PaneIdAllocator.h>
#include "Tabs/Panes/PaneId.h"

namespace winrt::GhosttyWin32::implementation {

// Core owns the implementation (Core/Panes/PaneIdAllocator.h); the
// alias keeps every existing spelling in App working.
using PaneIdAllocator = core::panes::PaneIdAllocator;

}  // namespace winrt::GhosttyWin32::implementation
