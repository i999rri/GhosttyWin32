#pragma once

namespace core::panes {

// How a split arranges its children — ghostty's own word for it
// (SplitTree's `Split.Layout`). "Layout", not "direction", because
// Horizontal names the axis the children line up on, not a way an
// arrow points: Horizontal = side by side (Left/Right live here,
// the bar is a vertical line), Vertical = stacked (Up/Down, the bar
// is horizontal). A value rather than a bare enum so a held layout
// answers for itself — `layout.IsHorizontal()` instead of an enum
// comparison at every call site (same idiom as RemoveResult).
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

}  // namespace core::panes
