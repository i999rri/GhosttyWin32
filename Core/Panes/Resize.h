#pragma once

#include <Panes/Layout.h>
#include <ghostty.h>
#include <optional>

namespace core::panes {

// RESIZE_SPLIT's request, typed at the boundary because typing it
// IS the decision: the keybind's arrow + magnitude become the
// layout of the split the arrow crosses (LEFT / RIGHT move a
// vertical boundary, so a horizontal split) and the signed distance
// the boundary moves in pixels, positive toward right / down — the
// first child grows. The sign carries the arrow, so no bool does;
// upstream's tree resize is fed the same way (a layout plus a
// signed ratio).
class Resize {
public:
    static constexpr std::optional<Resize> From(
        ghostty_action_resize_split_s resize) noexcept
    {
        const int amount = static_cast<int>(resize.amount);
        switch (resize.direction) {
        case GHOSTTY_RESIZE_SPLIT_RIGHT:
            return Resize{ core::panes::Layout::Horizontal(),  amount };
        case GHOSTTY_RESIZE_SPLIT_LEFT:
            return Resize{ core::panes::Layout::Horizontal(), -amount };
        case GHOSTTY_RESIZE_SPLIT_DOWN:
            return Resize{ core::panes::Layout::Vertical(),    amount };
        case GHOSTTY_RESIZE_SPLIT_UP:
            return Resize{ core::panes::Layout::Vertical(),   -amount };
        default:
            return std::nullopt;
        }
    }

    // (Qualified types below: the method name shadows the type
    // inside this class.)
    constexpr core::panes::Layout Layout() const noexcept { return m_layout; }
    constexpr int SignedAmount() const noexcept { return m_amount; }

    constexpr bool operator==(Resize const&) const noexcept = default;

private:
    constexpr Resize(core::panes::Layout layout, int amount) noexcept
        : m_layout(layout), m_amount(amount) {}
    core::panes::Layout m_layout;
    int m_amount;
};

}  // namespace core::panes
