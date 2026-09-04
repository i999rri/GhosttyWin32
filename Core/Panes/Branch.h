#pragma once

#include <Panes/Pane.h>
#include <Panes/Split.h>
#include <winrt/Windows.Foundation.h>
#include <functional>
#include <memory>
#include <utility>
#include <variant>

namespace core::panes {

// The recursive unit of a pane tree — either a Pane (leaf) or a
// Split (internal node with two Branch children). Move-disabled
// because parent back-pointers on children would be invalidated by
// relocation; the tree is reshaped through unique_ptr indirection.
// `arrangedRect` is written by the layout pass and read by the
// spatial rules (GOTO_SPLIT, RESIZE_SPLIT); it lives here rather
// than on the pane because a Split node has an extent too.
struct Branch {
    std::variant<Pane, Split> value;
    Branch* parent{ nullptr };
    winrt::Windows::Foundation::Rect arrangedRect{};

    Branch() = default;
    explicit Branch(Pane p) : value(std::move(p)) {}
    // Wire each child's parent back-pointer here so the invariant
    // holds even for callers that skip the MakeSplitBranch factory.
    explicit Branch(Split s) : value(std::move(s)) {
        if (auto* split = TryGet<Split>()) {
            if (split->left)  split->left ->parent = this;
            if (split->right) split->right->parent = this;
        }
    }

    Branch(Branch const&) = delete;
    Branch& operator=(Branch const&) = delete;
    Branch(Branch&&) = delete;
    Branch& operator=(Branch&&) = delete;

    // Templated so a future third variant doesn't need a new method.
    // A T outside the variant fails to compile.
    template<class T>
    bool Is() const noexcept { return std::holds_alternative<T>(value); }

    // Matches WinRT's try_as<T>() shape: nullptr if the branch holds
    // a different variant, so `if (auto* p = branch.TryGet<Pane>())`
    // combines check and use in one line.
    template<class T>       T* TryGet()       noexcept { return std::get_if<T>(&value); }
    template<class T> T const* TryGet() const noexcept { return std::get_if<T>(&value); }

    // Composite-style: each operates on the subtree rooted at `this`.
    // Named after Pane rather than "leaf" so callers speak the
    // terminal-domain vocabulary.
    bool AnyPaneMatches(std::function<bool(Pane const&)> const& pred) const;

    void ForEachPane(std::function<void(Pane&)> const& visitor);
    void ForEachPane(std::function<void(Pane const&)> const& visitor) const;

    Pane*       FindPaneBy(std::function<bool(Pane const&)> const& pred);
    Pane const* FindPaneBy(std::function<bool(Pane const&)> const& pred) const;

    // Returns the Branch wrapping `target`, or nullptr if the pane
    // isn't in this subtree. Callers need the wrapping Branch to
    // rewire tree links; they typically only have a Pane& in hand.
    Branch*       FindBranchOfPane(Pane const& target);
    Branch const* FindBranchOfPane(Pane const& target) const;
};

inline bool Branch::AnyPaneMatches(
    std::function<bool(Pane const&)> const& pred) const
{
    if (auto* p = TryGet<Pane>()) return pred(*p);
    if (auto* s = TryGet<Split>()) {
        return s->AnyOfChildren([&](Branch const& c) {
            return c.AnyPaneMatches(pred);
        });
    }
    return false;
}

inline void Branch::ForEachPane(std::function<void(Pane&)> const& visitor) {
    if (auto* p = TryGet<Pane>()) { visitor(*p); return; }
    if (auto* s = TryGet<Split>()) {
        s->ForEachChild([&](Branch& c) { c.ForEachPane(visitor); });
    }
}

inline void Branch::ForEachPane(std::function<void(Pane const&)> const& visitor) const {
    if (auto* p = TryGet<Pane>()) { visitor(*p); return; }
    if (auto* s = TryGet<Split>()) {
        s->ForEachChild([&](Branch const& c) { c.ForEachPane(visitor); });
    }
}

inline Pane* Branch::FindPaneBy(
    std::function<bool(Pane const&)> const& pred)
{
    if (auto* p = TryGet<Pane>()) return pred(*p) ? p : nullptr;
    if (auto* s = TryGet<Split>()) {
        return s->FirstChildResult([&](Branch& c) -> Pane* {
            return c.FindPaneBy(pred);
        });
    }
    return nullptr;
}

inline Pane const* Branch::FindPaneBy(
    std::function<bool(Pane const&)> const& pred) const
{
    if (auto* p = TryGet<Pane>()) return pred(*p) ? p : nullptr;
    if (auto* s = TryGet<Split>()) {
        return s->FirstChildResult([&](Branch const& c) -> Pane const* {
            return c.FindPaneBy(pred);
        });
    }
    return nullptr;
}

inline Branch* Branch::FindBranchOfPane(Pane const& target) {
    if (auto* p = TryGet<Pane>()) return (p == &target) ? this : nullptr;
    if (auto* s = TryGet<Split>()) {
        return s->FirstChildResult([&](Branch& c) -> Branch* {
            return c.FindBranchOfPane(target);
        });
    }
    return nullptr;
}

inline Branch const* Branch::FindBranchOfPane(Pane const& target) const {
    if (auto* p = TryGet<Pane>()) return (p == &target) ? this : nullptr;
    if (auto* s = TryGet<Split>()) {
        return s->FirstChildResult([&](Branch const& c) -> Branch const* {
            return c.FindBranchOfPane(target);
        });
    }
    return nullptr;
}

inline std::unique_ptr<Branch> MakePaneBranch(Pane pane)
{
    return std::make_unique<Branch>(std::move(pane));
}

inline std::unique_ptr<Branch> MakeSplitBranch(
    Split::Layout layout, double ratio,
    std::unique_ptr<Branch> left, std::unique_ptr<Branch> right)
{
    return std::make_unique<Branch>(
        Split{ layout, ratio, std::move(left), std::move(right) });
}

// NEW_SPLIT's construction path, shaped like the action itself:
// split `source`, toward `direction`, growing `newPane`. A split
// always has a pane that existed before it, so the source anchors
// the signature; layout and slot assignment are both derived from
// the one direction here, in the only place that needs to know —
// callers hand over roles and never touch slots. The Layout
// overload above stays for construction where no direction exists
// (restoring a saved tree, tests building shapes directly).
inline std::unique_ptr<Branch> MakeSplitBranch(
    std::unique_ptr<Branch> source,
    Split::Direction direction,
    std::unique_ptr<Branch> newPane,
    double ratio = 0.5)
{
    // The new pane lands on the side the arrow points at: LEFT / UP
    // is the first (left / top) slot, the source keeps the other.
    const bool newPaneFirst = direction.IsLeft() || direction.IsUp();
    return MakeSplitBranch(direction.Layout(), ratio,
                           newPaneFirst ? std::move(newPane) : std::move(source),
                           newPaneFirst ? std::move(source)  : std::move(newPane));
}

}  // namespace core::panes
