#pragma once

#include <memory>

namespace winrt::GhosttyWin32::implementation {

struct Branch;

// Split — an internal node in the pane tree. Divides its region into
// two subregions along `direction`, with the first child taking
// `ratio` of the total extent. Each child is another Branch (which is
// again either a single Pane or another Split, recursively).
//
// Direction here is the axis of the split *bar*, not the layout axis
// of the children — kept as-is from the legacy Pane class to avoid
// renaming every call site all at once, but semantically it means:
//
//   Horizontal : children are laid out side by side (bar is vertical)
//   Vertical   : children are stacked            (bar is horizontal)
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

    // Returns a pointer to the first child for which pred(*child) is
    // true, or nullptr if neither matches. Non-const overload only —
    // findable children have to be mutable for the callers who use
    // this (tree walkers rewiring the tree).
    template<class Pred>
    Branch* FindChildBy(Pred&& pred) {
        if (left  && pred(*left))  return left.get();
        if (right && pred(*right)) return right.get();
        return nullptr;
    }
    template<class Pred>
    Branch const* FindChildBy(Pred&& pred) const {
        if (left  && pred(*left))  return left.get();
        if (right && pred(*right)) return right.get();
        return nullptr;
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

// Clamp helper shared by ratio-setters (in Tree / SplitPanel mutation
// paths). Keeps both children meaningfully visible when the user
// drags a splitter to the edge.
constexpr double ClampSplitRatio(double r) noexcept {
    if (r < 0.05) return 0.05;
    if (r > 0.95) return 0.95;
    return r;
}

}  // namespace winrt::GhosttyWin32::implementation
