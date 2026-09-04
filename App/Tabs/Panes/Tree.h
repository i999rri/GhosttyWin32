#pragma once

#include <Panes/Tree.h>
#include "Tabs/Panes/Branch.h"

namespace winrt::GhosttyWin32::implementation {

// Core owns the model (Core/Panes/Tree.h, unit-tested in
// test_tree.cpp); the alias keeps every existing spelling in App
// working — SplitPanel owns a Tree and syncs its Children collection
// after each mutation, exactly as before.
using Tree = core::panes::Tree;

}  // namespace winrt::GhosttyWin32::implementation
