#pragma once

#include "Tabs/Panes/Pane.h"
#include "Tabs/Panes/Split.h"
#include <winrt/Windows.Foundation.h>
#include <functional>
#include <variant>

namespace winrt::GhosttyWin32::implementation {

// The recursive unit of a pane tree — the "unknown-subtree" value.
// Every position in the tree (root, and each child of every Split) is
// a Branch, which is either a Pane (a single terminal, leaf-like) or a
// Split (an internal node that divides further). Reading a `Branch*`
// as "an unknown subtree" is deliberate: the caller doesn't have to
// know which variant they're holding until they visit.
//
// Naming follows the tree metaphor as widely used in software (git
// branches, filesystem branches): a branch is what grows from a node,
// and the tip of a branch is either another branch (further division)
// or the end (a single pane in our case). Pane vs Split names the
// terminal-domain concept at each end; Branch names the recursive
// structural unit.
//
// Move-disabled — parent back-pointers on children are raw Branch*s
// that would be invalidated by relocation. Unique-ptr indirection is
// how the tree is reshaped.
struct Branch {
    std::variant<Pane, Split> value;

    // Back-pointer to the enclosing Branch (which will be a Split
    // variant). Null at the tree's root. Set when this Branch is
    // attached as a child of a Split, cleared when detached.
    Branch* parent{ nullptr };

    // Most-recent rectangle this branch was arranged into, in
    // SplitPanel-local coordinates. Set by the layout pass; consumed
    // by splitter-drag resize math and direction-based GOTO_SPLIT.
    // Zero before the first arrange (callers treat that as "no info").
    winrt::Windows::Foundation::Rect arrangedRect{};

    Branch() = default;
    explicit Branch(Pane p) : value(std::move(p)) {}
    explicit Branch(Split s) : value(std::move(s)) {}

    Branch(Branch const&) = delete;
    Branch& operator=(Branch const&) = delete;
    Branch(Branch&&) = delete;
    Branch& operator=(Branch&&) = delete;

    // Discriminator — every call site that used to gate on
    // `pane->IsLeaf()` now asks the branch whether it holds a T.
    // Templated on the target variant so a future third alternative
    // wouldn't need a new named method (Is<Pane>() / Is<Split>()
    // are the current uses).
    template<class T>
    bool Is() const noexcept { return std::holds_alternative<T>(value); }

    // Nullable extract — thin wrapper over std::get_if that keeps
    // the branch-value indirection out of the call site and matches
    // the WinRT try_as<T>() shape used elsewhere in this codebase.
    // Returns nullptr when the branch holds a different variant, so
    // callers can combine check-and-use in one line:
    //     if (auto* p = branch.TryGet<Pane>()) { p->content; }
    // A T not in the variant fails to compile — safe against typos.
    template<class T>       T* TryGet()       noexcept { return std::get_if<T>(&value); }
    template<class T> T const* TryGet() const noexcept { return std::get_if<T>(&value); }

    // ─── walker primitives (Composite-style: operate on subtree rooted at `this`) ───
    //
    // Named after the terminal-domain concept ("Pane") rather than the
    // tree-theory word ("Leaf") — matches what callers actually mean
    // ("does any pane in this subtree need X?") without dragging a
    // plant metaphor into a terminal codebase.

    // True if any Pane in this subtree satisfies `pred`. Short-circuits
    // on the first match.
    bool AnyPaneMatches(std::function<bool(Pane const&)> const& pred) const;

    // Visit every Pane in this subtree in depth-first order. Mutable
    // access — mutating pane content is allowed (setting content, etc.);
    // mutating the tree shape from inside the visitor is not.
    void ForEachPane(std::function<void(Pane&)> const& visitor);
    void ForEachPane(std::function<void(Pane const&)> const& visitor) const;

    // First Pane in this subtree matching `pred`, or nullptr.
    Pane*       FindPane(std::function<bool(Pane const&)> const& pred);
    Pane const* FindPane(std::function<bool(Pane const&)> const& pred) const;

    // The enclosing Branch that carries a specific Pane back to its
    // owner. Used when a pane needs to be replaced/removed and the
    // caller only has a Pane* — the tree walker locates the Branch
    // wrapping it so the unique_ptr can be rewired.
    Branch*       FindBranchOfPane(Pane const* target);
    Branch const* FindBranchOfPane(Pane const* target) const;
};

// ─── inline implementations ───
// Header-only so the templates and const/non-const overloads don't
// need a companion .cpp; the recursion is small.

inline bool Branch::AnyPaneMatches(
    std::function<bool(Pane const&)> const& pred) const
{
    if (auto* p = TryGet<Pane>()) return pred(*p);
    if (auto* s = TryGet<Split>()) {
        return (s->left  && s->left ->AnyPaneMatches(pred))
            || (s->right && s->right->AnyPaneMatches(pred));
    }
    return false;
}

inline void Branch::ForEachPane(std::function<void(Pane&)> const& visitor) {
    if (auto* p = TryGet<Pane>()) { visitor(*p); return; }
    if (auto* s = TryGet<Split>()) {
        if (s->left)  s->left ->ForEachPane(visitor);
        if (s->right) s->right->ForEachPane(visitor);
    }
}

inline void Branch::ForEachPane(std::function<void(Pane const&)> const& visitor) const {
    if (auto* p = TryGet<Pane>()) { visitor(*p); return; }
    if (auto* s = TryGet<Split>()) {
        if (s->left)  s->left ->ForEachPane(visitor);
        if (s->right) s->right->ForEachPane(visitor);
    }
}

inline Pane* Branch::FindPane(
    std::function<bool(Pane const&)> const& pred)
{
    if (auto* p = TryGet<Pane>()) return pred(*p) ? p : nullptr;
    if (auto* s = TryGet<Split>()) {
        if (s->left)  if (auto* hit = s->left->FindPane(pred))  return hit;
        if (s->right) if (auto* hit = s->right->FindPane(pred)) return hit;
    }
    return nullptr;
}

inline Pane const* Branch::FindPane(
    std::function<bool(Pane const&)> const& pred) const
{
    if (auto* p = TryGet<Pane>()) return pred(*p) ? p : nullptr;
    if (auto* s = TryGet<Split>()) {
        if (s->left)  if (auto* hit = s->left->FindPane(pred))  return hit;
        if (s->right) if (auto* hit = s->right->FindPane(pred)) return hit;
    }
    return nullptr;
}

inline Branch* Branch::FindBranchOfPane(Pane const* target) {
    if (!target) return nullptr;
    if (auto* p = TryGet<Pane>()) return (p == target) ? this : nullptr;
    if (auto* s = TryGet<Split>()) {
        if (s->left)  if (auto* hit = s->left ->FindBranchOfPane(target)) return hit;
        if (s->right) if (auto* hit = s->right->FindBranchOfPane(target)) return hit;
    }
    return nullptr;
}

inline Branch const* Branch::FindBranchOfPane(Pane const* target) const {
    if (!target) return nullptr;
    if (auto* p = TryGet<Pane>()) return (p == target) ? this : nullptr;
    if (auto* s = TryGet<Split>()) {
        if (s->left)  if (auto* hit = s->left ->FindBranchOfPane(target)) return hit;
        if (s->right) if (auto* hit = s->right->FindBranchOfPane(target)) return hit;
    }
    return nullptr;
}

// ─── factories (kept as free functions so Branch itself stays a
// plain aggregate over `value + parent + arrangedRect`) ───

// Wrap a leaf pane in a Branch on the heap. Callers rarely construct
// Branches directly — this factory keeps the parent/arrangedRect
// defaults implicit at the call site.
inline std::unique_ptr<Branch> MakePaneBranch(
    winrt::Microsoft::UI::Xaml::UIElement content, PaneId id = {})
{
    return std::make_unique<Branch>(Pane{ std::move(content), id });
}

// Build a split subtree by combining two existing branches. The
// parent back-pointers on the children are rewritten to point at the
// new Branch. Ratio is clamped so both children stay meaningfully
// visible.
inline std::unique_ptr<Branch> MakeSplitBranch(
    Split::Direction direction, double ratio,
    std::unique_ptr<Branch> left, std::unique_ptr<Branch> right)
{
    auto branch = std::make_unique<Branch>(Split{
        direction, ClampSplitRatio(ratio), std::move(left), std::move(right)
    });
    auto* split = branch->TryGet<Split>();
    if (split->left)  split->left ->parent = branch.get();
    if (split->right) split->right->parent = branch.get();
    return branch;
}

}  // namespace winrt::GhosttyWin32::implementation
