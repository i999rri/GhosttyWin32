#pragma once

#include <winrt/Microsoft.UI.Xaml.h>
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
    // collection when the leaf is arranged.
    static std::unique_ptr<Pane> MakeLeaf(Microsoft::UI::Xaml::UIElement content) {
        auto p = std::unique_ptr<Pane>(new Pane{});
        p->m_content = std::move(content);
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

private:
    Pane() = default;

    static double ClampRatio(double r) noexcept {
        if (r < 0.05) return 0.05;
        if (r > 0.95) return 0.95;
        return r;
    }

    // Leaf payload — non-null iff IsLeaf().
    Microsoft::UI::Xaml::UIElement m_content{ nullptr };

    // Internal-node fields — unused when IsLeaf().
    SplitOrientation m_orientation{ SplitOrientation::Horizontal };
    double m_ratio{ 0.5 };
    std::unique_ptr<Pane> m_first;
    std::unique_ptr<Pane> m_second;

    // Set by MakeSplit when this node is attached as a child; null at
    // the root. Used by future phases for direction-based navigation
    // and for collapsing the tree on pane close.
    Pane* m_parent{ nullptr };
};

}  // namespace winrt::GhosttyWin32::implementation
