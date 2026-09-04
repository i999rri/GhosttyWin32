#pragma once

#include <Panes/Split.h>

namespace winrt::GhosttyWin32::implementation {

// Core owns the node types (Core/Panes/Split.h); the aliases keep
// every existing spelling in App working.
using Split = core::panes::Split;
using core::panes::ClampSplitRatio;

}  // namespace winrt::GhosttyWin32::implementation
