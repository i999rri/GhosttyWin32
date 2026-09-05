#pragma once

#include <Panes/Direction.h>
#include <Panes/Layout.h>
#include <Panes/Split.h>

namespace winrt::GhosttyWin32::implementation {

// Core owns the node types (Core/Panes/*.h); the aliases keep
// every existing spelling in App working.
using Split = core::panes::Split;
using Layout = core::panes::Layout;
using Direction = core::panes::Direction;
using core::panes::ClampSplitRatio;

}  // namespace winrt::GhosttyWin32::implementation
