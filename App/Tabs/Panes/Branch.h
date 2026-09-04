#pragma once

#include <Panes/Branch.h>
#include "Tabs/Panes/Pane.h"
#include "Tabs/Panes/Split.h"

namespace winrt::GhosttyWin32::implementation {

// Core owns the node types (Core/Panes/Branch.h); the aliases keep
// every existing spelling in App working, including the branch
// factories (MakePaneBranch takes a Pane — copying one shares the
// control, which is what a split re-wrap relies on).
using Branch = core::panes::Branch;
using core::panes::MakePaneBranch;
using core::panes::MakeSplitBranch;

}  // namespace winrt::GhosttyWin32::implementation
