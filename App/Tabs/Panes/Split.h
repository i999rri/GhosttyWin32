#pragma once

#include <memory>

namespace winrt::GhosttyWin32::implementation {

struct Branch;

// Clamp helper shared by ratio-setters (in Tree / SplitPanel mutation
// paths) and by the Split constructor. Keeps both children
// meaningfully visible when the user drags a splitter to the edge.
constexpr double ClampSplitRatio(double r) noexcept {
    if (r < 0.05) return 0.05;
    if (r > 0.95) return 0.95;
    return r;
}

// Split — an internal node in the pane tree. Divides its region into
// two subregions along `direction`, with the first child taking
// `ratio` of the total extent. Each child is another Branch (which is
// again either a single Pane or another Split, recursively).
//
// Direction names the axis of the split *bar*, not the layout axis
// of the children:
//
//   Horizontal : children are laid out side by side (bar is vertical)
//   Vertical   : children are stacked               (bar is horizontal)
//
// A Split isn't move-enabled — Branch back-pointers into left/right
// children stay valid for the split's lifetime, and moving would
// invalidate them.
struct Split {
    enum class Direction {
        Horizontal,
        Vertical,
    };

    Direction direction{ Direction::Horizontal };
    double    ratio{ 0.5 };

    // Owned children. Both non-null in a well-formed tree; nullptr is
    // only ever a transient state during mutation.
    //
    // Named left / right because the layout code (SplitPanel Measure /
    // Arrange) cares which side of the split axis a child sits on.
    // Walker code that just needs "visit both children" should reach
    // for AnyOfChildren / FindChild / ForEachChild below — those
    // centralise the left+right iteration so callers don't repeat
    // the pattern for every new walker.
    std::unique_ptr<Branch> left;
    std::unique_ptr<Branch> right;

    Split() = default;
    // Full-init constructor. Clamps `r` into the safe range so bad
    // ratios can't sneak in via direct construction — the invariant
    // lives on the type, not on the MakeSplitBranch factory.
    Split(Direction d, double r,
          std::unique_ptr<Branch> l,
          std::unique_ptr<Branch> right_)
        : direction(d)
        , ratio(ClampSplitRatio(r))
        , left(std::move(l))
        , right(std::move(right_)) {}

    // Direction predicates. Reads as `split->IsHorizontal()` at the
    // call site instead of `split->direction == Direction::Horizontal`
    // — same semantic, English clause style consistent with the
    // Is<T>() / HasRoot() predicates elsewhere in this layer.
    bool IsHorizontal() const noexcept { return direction == Direction::Horizontal; }
    bool IsVertical()   const noexcept { return direction == Direction::Vertical; }

    // ─── child iteration helpers (used by Branch walker methods) ───
    //
    // Template + inline: the parameter pack references Branch::methods
    // that get resolved at the call site (Branch is fully defined by
    // the time any real caller includes Branch.h — which includes
    // this header — and instantiates one of these).

    // Short-circuiting "any": returns true on the first child whose
    // pred(*child) returns true. Missing children (transient nullptr
    // during mutation) are skipped.
    template<class Pred>
    bool AnyOfChildren(Pred&& pred) const {
        return (left  && pred(*left))
            || (right && pred(*right));
    }

    // Applies fn to each present child in order (left, right) and
    // returns the first result that converts to true (non-null
    // pointer, engaged optional, etc.). Falls back to a default-
    // constructed R when no child yields one — nullptr for pointer
    // returns, empty for optional returns. Mirrors Rust's find_map:
    // callers propagate a value the child produced (a Pane* found
    // deeper in the subtree, say) up through the composite without
    // routing it via a captured outer variable.
    template<class F>
    auto FirstChildResult(F&& fn) -> decltype(fn(std::declval<Branch&>())) {
        using R = decltype(fn(std::declval<Branch&>()));
        if (left)  { if (auto r = fn(*left);  r) return r; }
        if (right) { if (auto r = fn(*right); r) return r; }
        return R{};
    }
    template<class F>
    auto FirstChildResult(F&& fn) const -> decltype(fn(std::declval<Branch const&>())) {
        using R = decltype(fn(std::declval<Branch const&>()));
        if (left)  { if (auto r = fn(*left);  r) return r; }
        if (right) { if (auto r = fn(*right); r) return r; }
        return R{};
    }

    // Non-short-circuit visit — calls fn on every present child.
    // Callers who care about a specific direction or need early exit
    // should use AnyOfChildren / FindChild instead.
    template<class F>
    void ForEachChild(F&& fn) {
        if (left)  fn(*left);
        if (right) fn(*right);
    }
    template<class F>
    void ForEachChild(F&& fn) const {
        if (left)  fn(*left);
        if (right) fn(*right);
    }
};

}  // namespace winrt::GhosttyWin32::implementation
