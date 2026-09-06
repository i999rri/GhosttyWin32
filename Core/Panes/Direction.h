#pragma once

#include <Panes/Layout.h>
#include <ghostty.h>
#include <optional>

namespace core::panes {

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
    // put panes side by side, Up/Down stack them. (Qualified return
    // type: the method name shadows the type inside this class.)
    constexpr core::panes::Layout Layout() const noexcept {
        return (m_kind == Kind::Left || m_kind == Kind::Right)
            ? core::panes::Layout::Horizontal()
            : core::panes::Layout::Vertical();
    }
    constexpr bool IsHorizontal() const noexcept { return Layout().IsHorizontal(); }
    constexpr bool IsVertical()   const noexcept { return Layout().IsVertical(); }

    constexpr bool operator==(Direction const&) const noexcept = default;

private:
    enum class Kind { Left, Right, Up, Down };
    constexpr Direction(Kind k) noexcept : m_kind(k) {}
    Kind m_kind;
};

}  // namespace core::panes
