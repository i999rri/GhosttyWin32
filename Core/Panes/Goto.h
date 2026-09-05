#pragma once

#include <ghostty.h>
#include <optional>

namespace core::panes {

// GOTO_SPLIT's target (upstream SplitTree.Goto): a step through the
// panes in depth-first order (Previous / Next, wrapping at either
// end) or a spatial arrow to the nearest pane on that side. Unlike
// Resize there is no conversion in here — the value exists so
// consumers read predicates (target.IsNext()) instead of spelling
// enum comparisons at every branch.
class Goto {
public:
    static constexpr Goto Previous() noexcept { return { Kind::Previous }; }
    static constexpr Goto Next()     noexcept { return { Kind::Next }; }
    static constexpr Goto Left()     noexcept { return { Kind::Left }; }
    static constexpr Goto Right()    noexcept { return { Kind::Right }; }
    static constexpr Goto Up()       noexcept { return { Kind::Up }; }
    static constexpr Goto Down()     noexcept { return { Kind::Down }; }

    // The ghostty action enum, validated: any other value is a
    // target we don't model, not a default.
    static constexpr std::optional<Goto> From(
        ghostty_action_goto_split_e target) noexcept
    {
        switch (target) {
        case GHOSTTY_GOTO_SPLIT_PREVIOUS: return Previous();
        case GHOSTTY_GOTO_SPLIT_NEXT:     return Next();
        case GHOSTTY_GOTO_SPLIT_LEFT:     return Left();
        case GHOSTTY_GOTO_SPLIT_RIGHT:    return Right();
        case GHOSTTY_GOTO_SPLIT_UP:       return Up();
        case GHOSTTY_GOTO_SPLIT_DOWN:     return Down();
        default:                          return std::nullopt;
        }
    }

    constexpr bool IsPrevious() const noexcept { return m_kind == Kind::Previous; }
    constexpr bool IsNext()     const noexcept { return m_kind == Kind::Next; }
    constexpr bool IsLeft()     const noexcept { return m_kind == Kind::Left; }
    constexpr bool IsRight()    const noexcept { return m_kind == Kind::Right; }
    constexpr bool IsUp()       const noexcept { return m_kind == Kind::Up; }
    constexpr bool IsDown()     const noexcept { return m_kind == Kind::Down; }

    constexpr bool operator==(Goto const&) const noexcept = default;

private:
    enum class Kind { Previous, Next, Left, Right, Up, Down };
    constexpr Goto(Kind k) noexcept : m_kind(k) {}
    Kind m_kind;
};

}  // namespace core::panes
