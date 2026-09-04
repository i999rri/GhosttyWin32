#pragma once

#include <Panes/Pane.h>
#include "Tabs/Panes/PaneId.h"
#include "Terminal/TerminalControl.xaml.h"

namespace winrt::GhosttyWin32::implementation {

// The tree's leaf. Core owns the type (Core/Panes/Pane.h): a pane
// carries its control as an IInspectable `handle` (ownership) plus
// an IPaneView `view` (what the pane's owners do with it), so the
// tree never names TerminalControl and lives in Core. In this app
// both refer to the same TerminalControl; TabFactory sets them when
// it makes the pane.
using Pane = core::panes::Pane;

// The concrete control behind a pane, for the few callers that need
// TerminalControl-only methods (IME notifications, composition
// handle, the search box, InnerPanel). The one sanctioned downcast:
// this app puts exactly one kind of view into panes, so `view` is
// always a TerminalControl. Borrowed — `handle` keeps it alive.
inline TerminalControl* ControlOf(Pane const& p) noexcept {
    return p.view ? static_cast<TerminalControl*>(p.view) : nullptr;
}

}  // namespace winrt::GhosttyWin32::implementation
