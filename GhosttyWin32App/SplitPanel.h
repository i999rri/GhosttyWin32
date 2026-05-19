#pragma once

#include "SplitPanel.g.h"
#include "Pane.h"
#include <memory>

namespace winrt::GhosttyWin32::implementation {

// Custom Panel that lays out a Pane tree.
//
// The Pane tree is the source of truth for geometry: each internal
// node holds an orientation + ratio, each leaf wraps a UIElement.
// SplitPanel owns the root (`m_root`); MeasureOverride / ArrangeOverride
// recurse the tree and split the available rectangle accordingly.
//
// Children registration with the underlying Panel is handled by
// SetRoot(): every leaf's `Content()` UIElement is appended to
// `Children()` so the framework's measure / arrange machinery and
// hit-testing see them. The XAML tree shape is intentionally flat —
// no nested Panels, no GridSplitter — so leaves don't pay for
// transform stacks they don't need, and a future "swap two leaves"
// operation reduces to swapping unique_ptrs in the tree without
// touching XAML beyond a Measure invalidation.
struct SplitPanel : SplitPanelT<SplitPanel> {
    SplitPanel() = default;

    // Replaces the current Pane tree with `root` and refreshes the
    // Panel's children to contain exactly the leaf UIElements (in
    // depth-first traversal order). Safe to call repeatedly; the old
    // tree is destroyed, the new one takes effect on the next layout
    // pass.
    //
    // Passing `nullptr` clears the panel: tree gone, no children.
    void SetRoot(std::unique_ptr<Pane> root);

    // Read-only access for the host (Tab will need this to walk for
    // active-leaf focusing, but Phase 1 only uses it for diagnostics).
    Pane* Root() const noexcept { return m_root.get(); }

    // Panel overrides.
    Windows::Foundation::Size MeasureOverride(Windows::Foundation::Size availableSize);
    Windows::Foundation::Size ArrangeOverride(Windows::Foundation::Size finalSize);

private:
    // Recursive arrange — `rect` is the area assigned to `node` in
    // SplitPanel coordinates, before any nested splits apply.
    void ArrangeNode(Pane& node, Windows::Foundation::Rect rect);

    // Repopulates `Children()` to match the current tree (depth-first
    // leaf order). Called by SetRoot after the tree pointer swap.
    void SyncChildrenFromTree();

    // Append every leaf under `node` to `Children()`. Recursive helper
    // for SyncChildrenFromTree.
    void AppendLeavesToChildren(Pane& node);

    std::unique_ptr<Pane> m_root;
};

}  // namespace winrt::GhosttyWin32::implementation

namespace winrt::GhosttyWin32::factory_implementation {

struct SplitPanel : SplitPanelT<SplitPanel, implementation::SplitPanel> {};

}  // namespace winrt::GhosttyWin32::factory_implementation
