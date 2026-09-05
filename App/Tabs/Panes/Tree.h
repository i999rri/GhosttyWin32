#pragma once

#include <Panes/Goto.h>
#include <Panes/Resize.h>
#include <Panes/Tree.h>
#include "Tabs/Panes/Branch.h"

namespace winrt::GhosttyWin32::implementation {

// Core owns the model (Core/Panes/Tree.h, unit-tested in
// test_tree.cpp); the alias keeps every existing spelling in App
// working — SplitPanel owns a Tree and syncs its Children collection
// after each mutation, exactly as before.
using Tree = core::panes::Tree;
using Goto = core::panes::Goto;
using RemoveResult = core::panes::RemoveResult;
using Resize = core::panes::Resize;

}  // namespace winrt::GhosttyWin32::implementation
