#pragma once

#include "SplitPanel.g.h"
#include "Pane.h"
#include <memory>
#include <vector>

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

    // Replace `leaf` (which must currently be reachable from m_root)
    // with the subtree `newSubtree`. Returns true on success — false
    // if `leaf` isn't in this tree, or if either pointer is null.
    //
    // Used by NEW_SPLIT: the caller builds a split subtree whose
    // first/second is a new leaf wrapping the existing leaf's content
    // (preserving its PaneId so close_surface_cb still routes
    // correctly) plus a fresh leaf for the new pane, then calls this
    // to swap it in. The framework's Children() collection is
    // refreshed and a layout pass is requested.
    bool ReplaceLeaf(Pane* leaf, std::unique_ptr<Pane> newSubtree);

    // Remove `leaf` from the tree and collapse its enclosing split
    // node, promoting the surviving sibling into the slot the parent
    // occupied. Returns the kind of removal that happened so the
    // caller can take additional UI action:
    //   * RemovedRoot — leaf was the root; tree is now empty (the
    //     caller is expected to close the surrounding tab).
    //   * Collapsed   — leaf had a sibling; parent split node was
    //     replaced with that sibling, tab continues to render.
    //   * NotFound    — leaf wasn't reachable from m_root, or one of
    //     the back-pointer invariants was broken; no mutation
    //     happened.
    //
    // The leaf's TerminalControl is NOT detached here; the caller is
    // expected to do that before invoking RemoveLeaf so the surface
    // and DComp handle are released synchronously.
    enum class RemovalResult {
        NotFound,
        RemovedRoot,
        Collapsed,
    };
    RemovalResult RemoveLeaf(Pane* leaf);

    // Reset every internal node's ratio to 0.5 (so each split divides
    // its area evenly) and trigger a layout pass. Matches what the
    // ghostty EQUALIZE_SPLITS action expects — no-op on a single-leaf
    // tree.
    void EqualizeAll();

    // Read-only access for the host (Tab will need this to walk for
    // active-leaf focusing, but Phase 1 only uses it for diagnostics).
    Pane* Root() const noexcept { return m_root.get(); }

    // Panel overrides.
    Windows::Foundation::Size MeasureOverride(Windows::Foundation::Size availableSize);
    Windows::Foundation::Size ArrangeOverride(Windows::Foundation::Size finalSize);

    // Width of the draggable splitter strip in DIPs. Wide enough to
    // hit reliably with mouse, narrow enough that it doesn't visually
    // dominate the split — matches Windows Terminal's GridSplitter.
    static constexpr double kSplitterThickness = 6.0;

private:
    // Recursive measure — caps each subtree at its share of `available`
    // along the split axis. Called by MeasureOverride.
    Windows::Foundation::Size MeasureNode(Pane& node, Windows::Foundation::Size available);

    // Recursive arrange — `rect` is the area assigned to `node` in
    // SplitPanel coordinates, before any nested splits apply.
    void ArrangeNode(Pane& node, Windows::Foundation::Rect rect);

    // Repopulates `Children()` (and the parallel `m_splitters` list)
    // to match the current tree. Called by SetRoot / ReplaceLeaf /
    // RemoveLeaf after a tree mutation.
    void SyncChildrenFromTree();

    // Append a depth-first traversal of `node` to `Children()`: every
    // leaf's content, and one fresh Splitter Border per internal node
    // (recorded in m_splitters so MeasureNode / ArrangeNode can reach
    // it from the corresponding Pane*).
    void AppendNodeToChildren(Pane& node);

    // Build a Border for the drag-handle of an internal node and wire
    // its pointer events. Borders are recreated on every
    // SyncChildrenFromTree, so the captured node pointer is always
    // current at the time the lambda runs.
    Microsoft::UI::Xaml::Controls::Border MakeSplitter(Pane* node);

    // Find the Border previously created for `node`, or nullptr if
    // none. Used during measure/arrange to position the strip.
    Microsoft::UI::Xaml::Controls::Border SplitterForNode(Pane const* node) const;

    // Pointer event handlers. Called from the lambdas wired up in
    // MakeSplitter. `splitter` is the Border that owns the event;
    // `node` is the internal Pane the splitter is for.
    void OnSplitterPointerPressed(Microsoft::UI::Xaml::UIElement const& splitter,
                                  Pane* node,
                                  Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnSplitterPointerMoved(Pane* node,
                                Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnSplitterPointerReleased(Microsoft::UI::Xaml::UIElement const& splitter,
                                   Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);

    struct SplitterEntry {
        Microsoft::UI::Xaml::Controls::Border element{ nullptr };
        Pane* node{ nullptr };
    };

    std::unique_ptr<Pane> m_root;
    std::vector<SplitterEntry> m_splitters;
    // Set while a splitter drag is in progress (PointerPressed →
    // PointerReleased / CaptureLost). Identifies which internal node's
    // ratio is being updated by PointerMoved.
    Pane* m_draggingNode{ nullptr };
};

}  // namespace winrt::GhosttyWin32::implementation

namespace winrt::GhosttyWin32::factory_implementation {

struct SplitPanel : SplitPanelT<SplitPanel, implementation::SplitPanel> {};

}  // namespace winrt::GhosttyWin32::factory_implementation
