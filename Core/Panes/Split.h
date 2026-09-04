#pragma once

#include <memory>
#include <utility>

namespace core::panes {

struct Branch;

// Clamp the ratio so both children stay visible even if the user
// drags a splitter all the way to the edge.
constexpr double ClampSplitRatio(double r) noexcept {
    if (r < 0.05) return 0.05;
    if (r > 0.95) return 0.95;
    return r;
}

// A Split isn't move-enabled — raw Branch back-pointers on children
// would be invalidated by relocation.
struct Split {
    enum class Direction {
        Horizontal,   // children side by side, bar is vertical
        Vertical,     // children stacked,       bar is horizontal
    };

    Direction direction{ Direction::Horizontal };
    double    ratio{ 0.5 };
    std::unique_ptr<Branch> left;
    std::unique_ptr<Branch> right;

    Split() = default;
    Split(Direction d, double r,
          std::unique_ptr<Branch> l,
          std::unique_ptr<Branch> right_)
        : direction(d)
        , ratio(ClampSplitRatio(r))
        , left(std::move(l))
        , right(std::move(right_)) {}

    bool IsHorizontal() const noexcept { return direction == Direction::Horizontal; }
    bool IsVertical()   const noexcept { return direction == Direction::Vertical; }

    // Templates instantiate at the call site, so Branch just needs to
    // be forward-declared here; Branch.h includes this header before
    // it defines the walker bodies.
    template<class Pred>
    bool AnyOfChildren(Pred&& pred) const {
        return (left  && pred(*left))
            || (right && pred(*right));
    }

    // Mirrors Rust's find_map: applies fn to each present child in
    // order and returns the first truthy result, otherwise a
    // default-constructed R.
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

}  // namespace core::panes
