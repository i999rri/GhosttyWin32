#pragma once

#include "SplitPanel.g.h"
#include "Tabs/Panes/Tree.h"
#include <memory>
#include <vector>

namespace winrt::GhosttyWin32::implementation {

// Custom Panel that lays out a pane tree.
//
// Owns the tree (via `Tree m_tree`) and renders it — MeasureOverride /
// ArrangeOverride walk each Branch and split the available rectangle
// according to the Split direction and ratio at each internal node.
// Leaves' UIElements are appended to the underlying Panel's Children()
// collection so framework input routing, hit-testing, and measure /
// arrange work end-to-end; the XAML shape stays flat (no nested
// Panels, no GridSplitter).
//
// This class straddles the model-view boundary:
//   * The tree data + walker + structural mutations live on `m_tree`
//     (pure C++, no WinUI, unit-testable in isolation).
//   * The XAML side of things — Children collection, MeasureOverride,
//     ArrangeOverride, splitter Borders, pointer-drag handling —
//     lives here on SplitPanel.
//
// SetRoot / ReplacePane / RemovePane are adapter methods: they call
// through to Tree's mutations, then refresh Children() and invalidate
// layout. Callers who only want to READ the tree go through Tree()
// directly (`splitPanel->Tree().AnyPaneMatches(...)`), skipping the
// XAML-side sync entirely.
struct SplitPanel : SplitPanelT<SplitPanel> {
    SplitPanel() = default;

    // Replaces the current tree with `root` and refreshes the Panel's
    // Children to contain exactly the leaf UIElements in depth-first
    // order. Safe to call repeatedly; the old tree is destroyed, the
    // new one takes effect on the next layout pass. Passing nullptr
    // clears the panel.
    void SetRoot(std::unique_ptr<Branch> root);

    // Replace the Branch wrapping `pane` with `newSubtree`. Returns
    // true on success; false if the pane isn't in this tree or either
    // pointer is null.
    //
    // Used by NEW_SPLIT: the caller builds a split subtree whose left
    // is a new Branch wrapping the existing pane's content (preserving
    // its PaneId so close_surface_cb still routes correctly) plus a
    // fresh Branch for the new pane, then calls this to swap it in.
    bool ReplacePane(Pane const* pane, std::unique_ptr<Branch> newSubtree);

    // Remove `pane` from the tree and collapse its enclosing split by
    // promoting the surviving sibling. Returns the kind of removal so
    // the caller can decide follow-up UI:
    //   * RemovedRoot — pane was the root; tree is now empty (caller
    //     is expected to close the surrounding tab).
    //   * Collapsed   — split collapsed onto sibling; tab continues.
    //   * NotFound    — pane wasn't in this tree; no mutation.
    //
    // The pane's TerminalControl is NOT detached here; the caller is
    // expected to do that before invoking RemovePane so the surface
    // and DComp handle are released synchronously.
    Tree::RemoveResult RemovePane(Pane const* pane);

    // Reset every Split node's ratio to 0.5 so each split divides
    // its area evenly. Matches EQUALIZE_SPLITS; no-op on a single-pane
    // tree.
    void EqualizeAll();

    // Zoom one pane to fill the entire SplitPanel — every other pane
    // and every splitter is hidden via Visibility=Collapsed. Pass
    // nullptr to unzoom. `pane` must be currently reachable from the
    // tree; on any tree mutation the zoom is automatically cleared
    // because the stored pointer can no longer be trusted.
    void SetZoomed(Pane const* pane);
    Pane const* Zoomed() const noexcept { return m_tree.Zoomed(); }

    // Read-only access to the underlying tree — walker calls go through
    // here (`splitPanel->Tree().AnyPaneMatches(...)`).
    class Tree&       Tree()       noexcept { return m_tree; }
    class Tree const& Tree() const noexcept { return m_tree; }

    // Panel overrides.
    winrt::Windows::Foundation::Size MeasureOverride(winrt::Windows::Foundation::Size availableSize);
    winrt::Windows::Foundation::Size ArrangeOverride(winrt::Windows::Foundation::Size finalSize);

    // Width of the splitter strip in DIPs. Doubles as the click
    // hit-target for drag-resize.
    static constexpr double kSplitterThickness = 1.0;

    // Set the brush used to paint the splitter strip. Called once by
    // TabFactory (config-derived colour) and re-callable if a config
    // reload changes split-divider-color.
    void SetDividerColor(winrt::Windows::UI::Color color) noexcept;

private:
    // Recursive measure — caps each subtree at its share of `available`
    // along the split axis.
    winrt::Windows::Foundation::Size MeasureBranch(Branch& branch, winrt::Windows::Foundation::Size available);

    // Recursive arrange — `rect` is the area assigned to `branch` in
    // SplitPanel coordinates, before any nested splits apply. Caches
    // the arranged rect on the Branch itself for later use (drag-resize,
    // direction-based GOTO_SPLIT).
    void ArrangeBranch(Branch& branch, winrt::Windows::Foundation::Rect rect);

    // Repopulates Children() and m_splitters to match the current tree.
    // Called after every SetRoot / ReplacePane / RemovePane.
    void SyncChildrenFromTree();

    // Append a depth-first traversal of `branch` to Children(): every
    // leaf's content, and one fresh Splitter Border per Split node
    // (recorded in m_splitters so measure/arrange can find it).
    void AppendBranchToChildren(Branch& branch);

    // Build a Border for the drag-handle of a Split branch and wire
    // its pointer events. Borders are recreated on every
    // SyncChildrenFromTree, so the captured Branch* is current at the
    // time the lambda runs.
    winrt::Microsoft::UI::Xaml::Controls::Border MakeSplitter(Branch* splitBranch);

    // Find the Border previously created for `splitBranch`, or nullptr.
    winrt::Microsoft::UI::Xaml::Controls::Border SplitterForBranch(Branch const* splitBranch) const;

    // Pointer event handlers. `splitBranch` is the Branch (holding a
    // Split) whose splitter is being manipulated.
    void OnSplitterPointerPressed(winrt::Microsoft::UI::Xaml::UIElement const& splitter,
                                  Branch* splitBranch,
                                  winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnSplitterPointerMoved(Branch* splitBranch,
                                winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnSplitterPointerReleased(winrt::Microsoft::UI::Xaml::UIElement const& splitter,
                                   winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);

    struct SplitterEntry {
        winrt::Microsoft::UI::Xaml::Controls::Border element{ nullptr };
        Branch* branch{ nullptr };  // always a Branch holding a Split
    };

    // Refresh Visibility on every child so the zoom state matches
    // Tree().Zoomed().
    void UpdateChildVisibility();

    class Tree m_tree;
    std::vector<SplitterEntry> m_splitters;
    // Cached brush handed to every splitter Border. Null before
    // TabFactory calls SetDividerColor — MakeSplitter then falls
    // back to a neutral semi-transparent gray so brand-new
    // SplitPanels are still visible.
    winrt::Microsoft::UI::Xaml::Media::SolidColorBrush m_dividerBrush{ nullptr };
    // Set while a splitter drag is in progress. Identifies which Split
    // branch's ratio is being updated by PointerMoved.
    Branch* m_draggingBranch{ nullptr };
};

}  // namespace winrt::GhosttyWin32::implementation

namespace winrt::GhosttyWin32::factory_implementation {

struct SplitPanel : SplitPanelT<SplitPanel, implementation::SplitPanel> {};

}  // namespace winrt::GhosttyWin32::factory_implementation
