#pragma once

#include "PaneId.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>
#include <memory>

namespace winrt::GhosttyWin32::implementation {

// Direction of a split node.
//
//   Horizontal : children laid out side by side, the split bar is vertical.
//   Vertical   : children stacked, the split bar is horizontal.
enum class SplitOrientation {
    Horizontal,
    Vertical,
};

// Binary-tree node describing how a Tab's content is partitioned into
// terminal panes. A node is either a leaf (one UI element fills the
// node's rectangle) or an internal node (two children, split by
// `orientation` at `ratio`).
//
// Leaves hold a plain `UIElement` rather than a `TerminalControl` so
// the same tree can host placeholder Borders during layout-only tests
// and real TerminalControls in production. SplitPanel only cares about
// "something to arrange", so erasing to the base class also keeps that
// renderer agnostic.
//
// Ownership is unique_ptr-based: a parent owns its two children, the
// SplitPanel owns the root. Parent back-pointers (`m_parent`) are kept
// so direction-based navigation can walk up and across without a
// separate index; they're set when a node is attached as a child and
// cleared on detach.
//
// The class is move-disabled because raw `Pane*` back-pointers must
// stay valid for the node's lifetime — moving would invalidate every
// child's `m_parent`.
class Pane {
public:
    // Construct a leaf wrapping `content`. `content` is the actual
    // UIElement that will be added to the SplitPanel's Children
    // collection when the leaf is arranged. `id` identifies the leaf
    // for ghostty's close_surface_cb routing — required to be non-zero
    // for production callers; a default-constructed PaneId is accepted
    // (and exposed as Id()) so layout-only tests can build trees
    // without an allocator.
    static std::unique_ptr<Pane> MakeLeaf(Microsoft::UI::Xaml::UIElement content,
                                          PaneId id = {}) {
        auto p = std::unique_ptr<Pane>(new Pane{});
        p->m_content = std::move(content);
        p->m_id = id;
        return p;
    }

    // Construct an internal node by combining two existing subtrees.
    // `ratio` is the fraction of the available extent the first child
    // gets along the split axis (clamped to [0.05, 0.95] to keep both
    // children meaningfully visible).
    static std::unique_ptr<Pane> MakeSplit(
        SplitOrientation orientation,
        double ratio,
        std::unique_ptr<Pane> first,
        std::unique_ptr<Pane> second
    ) {
        auto p = std::unique_ptr<Pane>(new Pane{});
        p->m_orientation = orientation;
        p->m_ratio = ClampRatio(ratio);
        p->m_first = std::move(first);
        p->m_second = std::move(second);
        if (p->m_first) p->m_first->m_parent = p.get();
        if (p->m_second) p->m_second->m_parent = p.get();
        return p;
    }

    ~Pane() = default;
    Pane(const Pane&) = delete;
    Pane& operator=(const Pane&) = delete;
    Pane(Pane&&) = delete;
    Pane& operator=(Pane&&) = delete;

    bool IsLeaf() const noexcept { return static_cast<bool>(m_content); }

    // Leaf accessor. Returns null for internal nodes.
    Microsoft::UI::Xaml::UIElement Content() const noexcept { return m_content; }

    // Leaf-side identifier set at MakeLeaf time. Used as cfg.userdata
    // for ghostty's close_surface_cb so the host can route the
    // callback back to a specific leaf. Internal nodes have no ID
    // (returns the zero sentinel).
    PaneId Id() const noexcept { return m_id; }

    // Internal-node accessors. Calling these on a leaf returns
    // default-initialized values / nullptrs — callers should gate on
    // IsLeaf() first.
    SplitOrientation Orientation() const noexcept { return m_orientation; }
    double Ratio() const noexcept { return m_ratio; }
    void SetRatio(double ratio) noexcept { m_ratio = ClampRatio(ratio); }
    Pane* First() const noexcept { return m_first.get(); }
    Pane* Second() const noexcept { return m_second.get(); }

    // Back-pointer to the enclosing internal node, or null at the root.
    Pane* Parent() const noexcept { return m_parent; }

    // Most-recent rectangle this node was arranged into, in
    // SplitPanel-local coordinates. Set by SplitPanel::ArrangeNode on
    // every layout pass; consumed by splitter-drag resize math and
    // direction-based GOTO_SPLIT (finding the leaf adjacent to the
    // active one). Defaults to zero before the first arrange, which
    // callers treat as "no info" — the host should defer any rect-
    // dependent action until the user can see the panel anyway.
    Windows::Foundation::Rect ArrangedRect() const noexcept { return m_arrangedRect; }
    void SetArrangedRect(Windows::Foundation::Rect r) noexcept { m_arrangedRect = r; }

    // Swap the child unique_ptr matching `oldChild` for `newChild`.
    // Returns true when `oldChild` was one of this node's children
    // (the old subtree is destroyed when its unique_ptr is overwritten;
    // the new child's parent back-pointer is rewritten to point here).
    // No-op + false if called on a leaf or if `oldChild` isn't a
    // child — the caller is expected to verify membership when the
    // distinction matters.
    //
    // Used by SplitPanel::ReplaceLeaf for in-place tree edits like
    // NEW_SPLIT (replace a leaf with a split subtree wrapping it) and
    // CLOSE_PANE (replace a split with its surviving child).
    bool ReplaceChild(Pane* oldChild, std::unique_ptr<Pane> newChild) noexcept {
        if (IsLeaf() || !oldChild || !newChild) return false;
        if (m_first.get() == oldChild) {
            newChild->m_parent = this;
            m_first = std::move(newChild);
            return true;
        }
        if (m_second.get() == oldChild) {
            newChild->m_parent = this;
            m_second = std::move(newChild);
            return true;
        }
        return false;
    }

    // Pull `child` out of this internal node and return ownership.
    // The child becomes an orphan (parent back-pointer cleared) so
    // the caller can re-attach it elsewhere — typically as a
    // replacement for this node itself when collapsing a split.
    // Returns nullptr if called on a leaf or if `child` isn't a child.
    std::unique_ptr<Pane> DetachChild(Pane* child) noexcept {
        if (IsLeaf() || !child) return nullptr;
        if (m_first.get() == child) {
            m_first->m_parent = nullptr;
            return std::move(m_first);
        }
        if (m_second.get() == child) {
            m_second->m_parent = nullptr;
            return std::move(m_second);
        }
        return nullptr;
    }

private:
    Pane() = default;

    static double ClampRatio(double r) noexcept {
        if (r < 0.05) return 0.05;
        if (r > 0.95) return 0.95;
        return r;
    }

    // Leaf payload — non-null iff IsLeaf().
    Microsoft::UI::Xaml::UIElement m_content{ nullptr };

    // Leaf identifier — zero sentinel for internal nodes and for
    // layout-only test leaves built without an allocator.
    PaneId m_id{};

    // Internal-node fields — unused when IsLeaf().
    SplitOrientation m_orientation{ SplitOrientation::Horizontal };
    double m_ratio{ 0.5 };
    std::unique_ptr<Pane> m_first;
    std::unique_ptr<Pane> m_second;

    // Set by MakeSplit when this node is attached as a child; null at
    // the root. Used by future phases for direction-based navigation
    // and for collapsing the tree on pane close.
    Pane* m_parent{ nullptr };

    // Refreshed on every SplitPanel arrange pass — see ArrangedRect.
    Windows::Foundation::Rect m_arrangedRect{};
};

}  // namespace winrt::GhosttyWin32::implementation
