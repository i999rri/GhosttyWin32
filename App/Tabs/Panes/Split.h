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
    std::unique_ptr<Branch> left;
    std::unique_ptr<Branch> right;
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
