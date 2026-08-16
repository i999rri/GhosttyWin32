#pragma once

#include "Tabs/Panes/Branch.h"
#include <cassert>
#include <functional>
#include <memory>
#include <utility>

namespace winrt::GhosttyWin32::implementation {

// Container for a pane tree — the model layer for one tab's split
// arrangement. Owns the root Branch (nullable = empty tree) plus
// tree-wide state that doesn't belong on any single Branch: the
// currently-zoomed pane (if any), the mutations that need to swap
// the root pointer, etc.
//
// Tree is pure C++ — no WinUI, no Win32 — so its behaviour is
// unit-testable in isolation with fake pane trees. The XAML-side
// Panel (SplitPanel) holds a Tree as a member and drives
// MeasureOverride / ArrangeOverride off it; the two layers meet at
// the mutation methods on this class (SetRoot / ReplacePane /
// RemovePane), where SplitPanel refreshes its Children collection
// after Tree finishes rewiring pointers.
//
// This is the "SplitTree" of the Ghostty upstream Swift design (see
// external/ghostty/macos/Sources/Features/Splits/SplitTree.swift),
// adapted to C++ sum types (Branch = variant<Pane, Split>) instead
// of Swift indirect enums. Named Tree here rather than SplitTree —
// the "split" prefix would collide with the Split variant type
// (making 'SplitTree' read as 'tree of Splits' when in fact the tree
// contains a mix of Panes and Splits) and adds no information the
// namespace / member type doesn't already convey.
class Tree {
public:
    Tree() = default;
    explicit Tree(std::unique_ptr<Branch> root) noexcept
        : m_root(std::move(root)) {}

    Tree(Tree const&) = delete;
    Tree& operator=(Tree const&) = delete;
    Tree(Tree&&) = default;
    Tree& operator=(Tree&&) = default;

    // ─── root access ───
    // Positive predicate: this tree has a root Branch (equivalently:
    // at least one Pane lives in it). Used as the guard in the
    // walker delegations below and by the TryFindBranch lookup
    // that the structural mutations key off. Named for the specific
    // state ("root exists") rather than the generic "not empty" —
    // callers read as `HasRoot()` without wondering "empty of what?".
    bool HasRoot() const noexcept { return static_cast<bool>(m_root); }
    Branch*       Root()       noexcept { return m_root.get(); }
    Branch const* Root() const noexcept { return m_root.get(); }

    // Replace the entire tree. Old subtree is destroyed as its
    // unique_ptr is overwritten. The new root's parent back-pointer
    // is cleared (a root has no parent).
    void SetRoot(std::unique_ptr<Branch> root) noexcept {
        if (root) root->parent = nullptr;
        m_root = std::move(root);
        // Zoom state can't survive a root replacement — the pointer
        // targets nodes that just got dropped.
        m_zoomed = nullptr;
    }

    // ─── walker delegation ───
    // Thin wrappers around Branch's Composite-style methods. Empty
    // trees answer conservatively (no matches, nothing to visit).

    bool AnyPaneMatches(std::function<bool(Pane const&)> const& pred) const {
        return HasRoot() && m_root->AnyPaneMatches(pred);
    }
    void ForEachPane(std::function<void(Pane&)> const& visitor) {
        if (HasRoot()) m_root->ForEachPane(visitor);
    }
    void ForEachPane(std::function<void(Pane const&)> const& visitor) const {
        if (HasRoot()) m_root->ForEachPane(visitor);
    }
    Pane* FindPaneBy(std::function<bool(Pane const&)> const& pred) {
        return HasRoot() ? m_root->FindPaneBy(pred) : nullptr;
    }
    Pane const* FindPaneBy(std::function<bool(Pane const&)> const& pred) const {
        return HasRoot() ? m_root->FindPaneBy(pred) : nullptr;
    }

    // ─── structural mutations ───

    // Locate the Branch that wraps `pane` in this tree, or nullptr
    // if the tree is empty OR the pane isn't in it. Both structural
    // mutations (ReplacePane, RemovePane) start the same way — find
    // the wrapping Branch or bail — so the two failure modes collapse
    // into one nullable return the callers dispatch on. `Try` +
    // `Find` matches the fallible-lookup convention we already use
    // for Branch::TryGet<T>().
    Branch* TryFindBranch(Pane const& pane) noexcept {
        return HasRoot() ? m_root->FindBranchOfPane(pane) : nullptr;
    }

    // Replace the Branch currently wrapping `target` with `newSubtree`.
    // Returns true on success; false only when `target` isn't in this
    // tree (a stale event, a Pane from a sibling window, etc.). Used
    // by NEW_SPLIT to swap a leaf for a split-of-that-leaf-plus-a-
    // new-leaf, preserving the existing pane's identity.
    //
    // Preconditions (checked by assert in debug builds; UB otherwise):
    //   * `target` is a real Pane — the reference parameter guarantees
    //     it up front, no runtime guard.
    //   * `newSubtree` is a non-null unique_ptr, i.e. the caller
    //     actually built one. Passing a default-constructed or
    //     moved-from unique_ptr is a programming bug — factory
    //     helpers (MakePaneBranch / MakeSplitBranch) always return
    //     non-null, so real call sites shouldn't hit the assert.
    //
    // The old subtree is destroyed as its unique_ptr is overwritten,
    // and `newSubtree`'s parent back-pointer is rewritten to match
    // the surrounding Split (or cleared, if `target`'s Branch was the
    // root).
    bool ReplacePane(Pane const& target, std::unique_ptr<Branch> newSubtree) noexcept {
        assert(newSubtree && "ReplacePane requires non-null newSubtree");
        Branch* wrapping = TryFindBranch(target);
        if (!wrapping) return false;

        // Was `target` the root? Swap m_root itself.
        if (wrapping == m_root.get()) {
            newSubtree->parent = nullptr;
            m_root = std::move(newSubtree);
            if (m_zoomed == &target) m_zoomed = nullptr;
            return true;
        }

        // Otherwise `wrapping` is a child of some Split. Find that
        // Split and rewire the matching unique_ptr slot.
        Branch* parent = wrapping->parent;
        if (!parent) return false;                       // shouldn't happen
        auto* parentSplit = parent->TryGet<Split>();
        if (!parentSplit) return false;                  // shouldn't happen

        newSubtree->parent = parent;
        if (parentSplit->left.get() == wrapping) {
            parentSplit->left = std::move(newSubtree);
        } else if (parentSplit->right.get() == wrapping) {
            parentSplit->right = std::move(newSubtree);
        } else {
            return false;                                // shouldn't happen
        }
        if (m_zoomed == &target) m_zoomed = nullptr;
        return true;
    }

    // Outcome of RemovePane — the caller keys UI reactions off which
    // case occurred (last-pane closes the whole tab, split-collapse
    // just relayouts, not-found is a stale close event).
    enum class RemoveResult {
        NotFound,      // `target` wasn't in this tree
        Collapsed,     // pane removed; its enclosing split collapsed onto its sibling
        RemovedRoot,   // pane was the tree's sole content; tree is now empty
    };

    // Remove the Pane `target` from the tree. If the pane was under a
    // Split, that split collapses and its surviving sibling is
    // promoted into the split's slot. `target` is by-reference —
    // callers must have a real Pane to name; a stale close event on
    // a pane already gone from this tree still returns NotFound.
    RemoveResult RemovePane(Pane const& target) noexcept {
        Branch* wrapping = TryFindBranch(target);
        if (!wrapping) return RemoveResult::NotFound;

        // Root case: the pane was the whole tree.
        if (wrapping == m_root.get()) {
            m_root.reset();
            m_zoomed = nullptr;
            return RemoveResult::RemovedRoot;
        }

        // Under a Split: promote the sibling into the split's slot.
        Branch* parent = wrapping->parent;
        if (!parent) return RemoveResult::NotFound;      // shouldn't happen
        auto* parentSplit = parent->TryGet<Split>();
        if (!parentSplit) return RemoveResult::NotFound; // shouldn't happen

        // Pick the sibling; detach it from the Split so we can hand
        // it over to the grandparent slot.
        std::unique_ptr<Branch> sibling;
        if (parentSplit->left.get() == wrapping) {
            sibling = std::move(parentSplit->right);
        } else if (parentSplit->right.get() == wrapping) {
            sibling = std::move(parentSplit->left);
        } else {
            return RemoveResult::NotFound;               // shouldn't happen
        }
        if (!sibling) return RemoveResult::NotFound;     // malformed split

        // Where does the split live? Root or under a grandparent Split.
        Branch* grand = parent->parent;
        if (!grand) {
            // Split was the root — sibling becomes the new root.
            sibling->parent = nullptr;
            m_root = std::move(sibling);
        } else {
            auto* grandSplit = grand->TryGet<Split>();
            if (!grandSplit) return RemoveResult::NotFound; // shouldn't happen
            sibling->parent = grand;
            if (grandSplit->left.get() == parent) {
                grandSplit->left = std::move(sibling);
            } else if (grandSplit->right.get() == parent) {
                grandSplit->right = std::move(sibling);
            } else {
                return RemoveResult::NotFound;           // shouldn't happen
            }
        }
        if (m_zoomed == &target) m_zoomed = nullptr;
        return RemoveResult::Collapsed;
    }

    // ─── zoom state ───
    // `toggle_split_zoom` action support: one pane in the tree can be
    // marked "zoomed", meaning it should take up the whole SplitPanel
    // area instead of participating in the split layout. Tree just
    // records which pane; SplitPanel consults `Zoomed()` in its
    // arrange pass and adjusts accordingly.

    Pane const* Zoomed() const noexcept { return m_zoomed; }
    void SetZoomed(Pane const* pane) noexcept { m_zoomed = pane; }
    void ClearZoomed() noexcept { m_zoomed = nullptr; }

private:
    std::unique_ptr<Branch> m_root;
    // Non-owning pointer into m_root's subtree. Cleared whenever the
    // pane it points at could be destroyed (SetRoot, ReplacePane or
    // RemovePane touching that pane).
    Pane const* m_zoomed{ nullptr };
};

}  // namespace winrt::GhosttyWin32::implementation
