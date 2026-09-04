#pragma once

#include <Host/IPaneView.h>
#include <Panes/PaneId.h>
#include <winrt/Windows.Foundation.h>

namespace core::panes {

// A single terminal — the tree's leaf. One pane hosts exactly one
// control, which in turn owns exactly one ghostty surface; that
// 1:1:1 chain is ghostty's own design premise (its split tree is
// SplitTree<SurfaceView>), not a host simplification.
//
// The control appears twice, split by role, so the tree never names
// the App's concrete type and can live in Core:
//
//   handle  keeps the control alive (WinRT ref count) and is what
//           the layout side turns back into a UIElement
//   view    what the pane's owners do with it — lifecycle, focus,
//           window-scoped looks (see IPaneView)
//
// Both refer to the same object; `view` is borrowed and valid while
// `handle` is held. Copying a pane copies the reference, not the
// control — the same sharing MakePaneBranch relies on when a split
// re-wraps an existing pane.
struct Pane {
    winrt::Windows::Foundation::IInspectable handle{ nullptr };
    host::IPaneView* view{ nullptr };
    PaneId id{};
};

}  // namespace core::panes
