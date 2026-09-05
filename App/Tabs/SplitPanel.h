#pragma once

#include "SplitPanel.g.h"
#include "Tabs/Panes/Tree.h"
#include "ghostty.h"
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
    // true on success; false if the pane isn't in this tree or
    // `newSubtree` is null. `pane` is by reference — non-null at the
    // type level.
    //
    // Used by NEW_SPLIT: the caller builds a split subtree whose left
    // is a new Branch wrapping the existing pane's content (preserving
    // its PaneId so close_surface_cb still routes correctly) plus a
    // fresh Branch for the new pane, then calls this to swap it in.
    bool ReplacePane(Pane const& pane, std::unique_ptr<Branch> newSubtree);

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
    Tree::RemoveResult RemovePane(Pane const& pane);

    // Reset every Split node's ratio to 0.5 so each split divides
    // its area evenly. Matches EQUALIZE_SPLITS; no-op on a single-pane
    // tree.
    void EqualizeAll();

    // ----- the split actions, over this panel's tree -----
    // Each takes the pane the action was fired on, asks the tree for
    // its answer (the rules live on Tree / Split, tested in
    // test_tree.cpp), and reports what changed. Focus and active-pane
    // bookkeeping are the caller's (MainWindow's) — they belong to
    // the tab, not the layout.

    // NEW_SPLIT: wrap `source` and `fresh` in a split, the new pane
    // on the side `direction` points at, keeping `source`'s control
    // and PaneId. Returns the
    // pane inside `fresh` (now in the tree), or null when `source` is
    // not in this tree — `fresh` is then destroyed unused, so a caller
    // that attached a surface to it must detach that first.
    Pane* SplitPane(Pane const& source,
                    Split::Direction direction,
                    std::unique_ptr<Branch> fresh);

    // GOTO_SPLIT: the pane focus should move to from `from` — the
    // spatial neighbour for LEFT / RIGHT / UP / DOWN, the depth-first
    // neighbour for PREVIOUS / NEXT — or null when there is none.
    Pane* PaneToward(Pane const& from, ghostty_action_goto_split_e direction);

    // RESIZE_SPLIT: move the boundary of the nearest split with the
    // request's layout. Returns false when no such split exists (a
    // lone pane, or only splits the other way). (Qualified parameter
    // type: Tree is also this panel's accessor name.)
    bool ResizeSplit(Pane const& pane, core::panes::Tree::Resize resize);

    // TOGGLE_SPLIT_ZOOM: a second press anywhere collapses an active
    // zoom (as Windows Terminal / iTerm do); a lone pane has nothing
    // to expand against; otherwise `pane` fills the panel. Returns
    // whether `pane` is zoomed afterwards.
    bool ToggleZoom(Pane const& pane);

    // Zoom one pane to fill the entire SplitPanel — every other pane
    // and every splitter is hidden via Visibility=Collapsed. Pass
    // nullptr to unzoom. `pane` must be currently reachable from the
    // tree; on any tree mutation the zoom is automatically cleared
    // because the stored pointer can no longer be trusted.
    void SetZoomed(Pane const* pane);
    Pane const* Zoomed() const noexcept { return m_tree.Zoomed(); }

    // Read-only access to the underlying tree — walker calls go through
    // here (`splitPanel->Tree().AnyPaneMatches(...)`).
    // Qualified, not `class Tree`: Tree is an alias of the Core type
    // now, and an elaborated-type-specifier cannot name an alias —
    // the qualified name sidesteps the method-name shadowing instead.
    core::panes::Tree&       Tree()       noexcept { return m_tree; }
    core::panes::Tree const& Tree() const noexcept { return m_tree; }

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

    core::panes::Tree m_tree;
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
