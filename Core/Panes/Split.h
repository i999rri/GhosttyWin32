#pragma once

#include <ghostty.h>
#include <memory>
#include <optional>
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
    // How this split arranges its children — ghostty's own word for
    // it (SplitTree's `Split.Layout`). "Layout", not "direction",
    // because Horizontal names the axis the children line up on, not
    // a way an arrow points: Horizontal = side by side (Left/Right
    // live here, the bar is a vertical line), Vertical = stacked
    // (Up/Down, the bar is horizontal). A value rather than a bare
    // enum so a held layout answers for itself —
    // `layout.IsHorizontal()` instead of an enum comparison at every
    // call site (same idiom as Tree::RemoveResult).
    class Layout {
    public:
        // children side by side, bar is vertical
        static constexpr Layout Horizontal() noexcept { return { Kind::Horizontal }; }
        // children stacked, bar is horizontal
        static constexpr Layout Vertical()   noexcept { return { Kind::Vertical }; }

        constexpr bool IsHorizontal() const noexcept { return m_kind == Kind::Horizontal; }
        constexpr bool IsVertical()   const noexcept { return m_kind == Kind::Vertical; }

        constexpr bool operator==(Layout const&) const noexcept = default;

    private:
        enum class Kind { Horizontal, Vertical };
        constexpr Layout(Kind k) noexcept : m_kind(k) {}
        Kind m_kind;
    };

    Layout layout{ Layout::Horizontal() };
    double    ratio{ 0.5 };
    std::unique_ptr<Branch> left;
    std::unique_ptr<Branch> right;

    Split() = default;
    Split(Layout layout_, double r,
          std::unique_ptr<Branch> l,
          std::unique_ptr<Branch> right_)
        : layout(layout_)
        , ratio(ClampSplitRatio(r))
        , left(std::move(l))
        , right(std::move(right_)) {}

    bool IsHorizontal() const noexcept { return layout.IsHorizontal(); }
    bool IsVertical()   const noexcept { return layout.IsVertical(); }

    // NEW_SPLIT: which side of the source the new pane is born on —
    // the four arrows of ghostty's split direction as one value. The
    // value answers what derives from the arrow (its Layout(), the
    // side predicates); which child takes which slot is derived from
    // it in one place, MakeSplitBranch's direction overload, so no
    // bare bool or positional pair ever crosses a call site.
    class Direction {
    public:
        static constexpr Direction Left()  noexcept { return { Kind::Left }; }
        static constexpr Direction Right() noexcept { return { Kind::Right }; }
        static constexpr Direction Up()    noexcept { return { Kind::Up }; }
        static constexpr Direction Down()  noexcept { return { Kind::Down }; }

        // The ghostty action enum, validated: any other value is a
        // direction we don't model, not a default.
        static constexpr std::optional<Direction> From(
            ghostty_action_split_direction_e direction) noexcept
        {
            switch (direction) {
            case GHOSTTY_SPLIT_DIRECTION_RIGHT: return Right();
            case GHOSTTY_SPLIT_DIRECTION_LEFT:  return Left();
            case GHOSTTY_SPLIT_DIRECTION_DOWN:  return Down();
            case GHOSTTY_SPLIT_DIRECTION_UP:    return Up();
            default:                            return std::nullopt;
            }
        }

        constexpr bool IsLeft()  const noexcept { return m_kind == Kind::Left; }
        constexpr bool IsRight() const noexcept { return m_kind == Kind::Right; }
        constexpr bool IsUp()    const noexcept { return m_kind == Kind::Up; }
        constexpr bool IsDown()  const noexcept { return m_kind == Kind::Down; }

        // The layout of the split this direction creates: Left/Right
        // put panes side by side, Up/Down stack them. (Qualified
        // return type: the method name shadows the type inside this
        // class.)
        constexpr Split::Layout Layout() const noexcept {
            return (m_kind == Kind::Left || m_kind == Kind::Right)
                ? Split::Layout::Horizontal()
                : Split::Layout::Vertical();
        }
        constexpr bool IsHorizontal() const noexcept { return Layout().IsHorizontal(); }
        constexpr bool IsVertical()   const noexcept { return Layout().IsVertical(); }

        constexpr bool operator==(Direction const&) const noexcept = default;

    private:
        enum class Kind { Left, Right, Up, Down };
        constexpr Direction(Kind k) noexcept : m_kind(k) {}
        Kind m_kind;
    };

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
