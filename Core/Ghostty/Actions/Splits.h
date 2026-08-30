#pragma once

#include "ghostty.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>

namespace core::ghostty::actions::splits {

// The decisions behind the split actions (NEW_SPLIT, GOTO_SPLIT,
// RESIZE_SPLIT), over nothing but rectangles, indices and ratios. The
// pane tree (App/Tabs/Panes) knows which pane is which and where it
// was arranged; it hands those facts in here and applies the answer.
// Nothing here sees a Pane, a Branch or a window, so every rule is
// unit-testable on its own.

// A pane's arranged rectangle, in whatever unit the layout used.
struct Rect {
    float x;
    float y;
    float width;
    float height;

    float right()   const noexcept { return x + width; }
    float bottom()  const noexcept { return y + height; }
    float centerX() const noexcept { return x + width * 0.5f; }
    float centerY() const noexcept { return y + height * 0.5f; }
};

// Which way a split divides its area. Mirrors the tree's
// Split::Direction without depending on it.
enum class Axis {
    Horizontal,   // children side by side
    Vertical,     // children stacked
};

// GOTO_SPLIT LEFT / RIGHT / UP / DOWN: the pane to move focus to, as
// an index into `panes`, or nullopt when no pane sits on that side.
//
// A candidate qualifies when its whole extent lies on the requested
// side of the active pane (with 1px of slack to absorb float rounding
// on a shared boundary). Among those, the score is
//   primary distance + 2 × perpendicular offset of the centres
// and the lowest wins: the 2× penalty keeps focus moves predictable
// when an off-axis pane is technically closer in straight-line
// distance than the aligned neighbour.
//
// PREVIOUS / NEXT are not spatial; see CyclePane.
inline std::optional<size_t> AdjacentPane(
    std::span<Rect const> panes,
    size_t activeIndex,
    ghostty_action_goto_split_e direction) noexcept
{
    if (activeIndex >= panes.size()) return std::nullopt;
    const Rect a = panes[activeIndex];

    std::optional<size_t> best;
    double bestScore = std::numeric_limits<double>::max();
    for (size_t i = 0; i < panes.size(); ++i) {
        if (i == activeIndex) continue;
        const Rect c = panes[i];

        double primary = 0.0, perpendicular = 0.0;
        switch (direction) {
        case GHOSTTY_GOTO_SPLIT_LEFT:
            if (c.right() > a.x + 1.0f) continue;
            primary = a.x - c.right();
            perpendicular = std::abs(c.centerY() - a.centerY());
            break;
        case GHOSTTY_GOTO_SPLIT_RIGHT:
            if (c.x < a.right() - 1.0f) continue;
            primary = c.x - a.right();
            perpendicular = std::abs(c.centerY() - a.centerY());
            break;
        case GHOSTTY_GOTO_SPLIT_UP:
            if (c.bottom() > a.y + 1.0f) continue;
            primary = a.y - c.bottom();
            perpendicular = std::abs(c.centerX() - a.centerX());
            break;
        case GHOSTTY_GOTO_SPLIT_DOWN:
            if (c.y < a.bottom() - 1.0f) continue;
            primary = c.y - a.bottom();
            perpendicular = std::abs(c.centerX() - a.centerX());
            break;
        default:
            return std::nullopt;
        }
        const double score = primary + 2.0 * perpendicular;
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

// GOTO_SPLIT PREVIOUS / NEXT: the neighbour in depth-first order,
// wrapping around at either end. Any other direction, or an index
// out of range, returns the index unchanged.
inline size_t CyclePane(size_t index, size_t count,
                        ghostty_action_goto_split_e direction) noexcept
{
    if (count == 0 || index >= count) return index;
    switch (direction) {
    case GHOSTTY_GOTO_SPLIT_NEXT:     return (index + 1) % count;
    case GHOSTTY_GOTO_SPLIT_PREVIOUS: return index == 0 ? count - 1 : index - 1;
    default:                          return index;
    }
}

// NEW_SPLIT: which axis the new split divides along, and whether the
// new pane goes before the existing one. RIGHT / DOWN put it after
// the source on the layout axis; LEFT / UP put it before.
struct Placement {
    Axis axis;
    bool newFirst;
};

inline std::optional<Placement> PlaceSplit(
    ghostty_action_split_direction_e direction) noexcept
{
    switch (direction) {
    case GHOSTTY_SPLIT_DIRECTION_RIGHT: return Placement{ Axis::Horizontal, false };
    case GHOSTTY_SPLIT_DIRECTION_LEFT:  return Placement{ Axis::Horizontal, true };
    case GHOSTTY_SPLIT_DIRECTION_DOWN:  return Placement{ Axis::Vertical,   false };
    case GHOSTTY_SPLIT_DIRECTION_UP:    return Placement{ Axis::Vertical,   true };
    default:                            return std::nullopt;
    }
}

// The size hint for the pane a split creates: the source's size
// halved along the split axis.
struct Size {
    uint32_t width;
    uint32_t height;
};

inline Size HalfAlong(Size source, Axis axis) noexcept
{
    return axis == Axis::Horizontal
        ? Size{ source.width / 2, source.height }
        : Size{ source.width,     source.height / 2 };
}

// RESIZE_SPLIT: the split axis the arrow refers to. LEFT / RIGHT move
// a vertical boundary, so the split being resized is a horizontal
// one; UP / DOWN the other way round.
inline Axis ResizeAxis(ghostty_action_resize_split_direction_e direction) noexcept
{
    return (direction == GHOSTTY_RESIZE_SPLIT_LEFT
            || direction == GHOSTTY_RESIZE_SPLIT_RIGHT)
        ? Axis::Horizontal
        : Axis::Vertical;
}

// The split's new ratio after moving its boundary by `amount` pixels
// in the arrow's direction. `extent` is the split's arranged length
// along its axis, `splitterThickness` what the divider takes of it.
// The arrow is the direction the boundary moves regardless of which
// side of the split the active pane is on: RIGHT / DOWN move it
// toward +axis, so the first child grows; LEFT / UP shrink it. The
// result is clamped to [0.05, 0.95] so neither child can vanish.
inline double ResizedRatio(double ratio,
                           ghostty_action_resize_split_s resize,
                           float extent,
                           float splitterThickness) noexcept
{
    const float usable = std::max(1.0f, extent - splitterThickness);
    const double delta = static_cast<double>(resize.amount) / usable;
    const bool increase = (resize.direction == GHOSTTY_RESIZE_SPLIT_RIGHT
                        || resize.direction == GHOSTTY_RESIZE_SPLIT_DOWN);
    return std::clamp(ratio + (increase ? delta : -delta), 0.05, 0.95);
}

}  // namespace core::ghostty::actions::splits
