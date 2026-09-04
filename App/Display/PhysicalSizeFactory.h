#pragma once

#include "Display/PhysicalPixels.h"
#include <Panes/Split.h>

namespace winrt::GhosttyWin32::implementation::display {

// Builds the PhysicalSize a new ghostty surface starts at. The value
// is a prediction — layout settles the real size right after — but a
// good prediction lets ghostty create the swap chain at the right
// size from the start instead of stretching and re-resizing on the
// first visible frame. All results are PHYSICAL pixels (what
// ghostty_surface_new expects; see PhysicalPixels.h for why the
// DIP conversion matters).
class PhysicalSizeFactory {
public:
    // For a pane born by splitting `source`: its measured size halved
    // along the split axis — the new pane gets one half of the area
    // the source occupied.
    template <typename TElement>
    static PhysicalSize ForSplit(TElement const& source,
                                 core::panes::Split::Direction direction)
    {
        auto size = MeasuredPhysical(source);
        if (direction.IsHorizontal()) {
            size.width /= 2;
        } else {
            size.height /= 2;
        }
        return size;
    }

    // For the pane of a brand-new tab, with nothing to measure yet:
    // the window content area's own size.
    template <typename TFallback>
    static PhysicalSize ForNewTab(TFallback const& content)
    {
        return MeasuredPhysical(content);
    }

    // For the pane of a brand-new tab: the active panel's measured
    // size — the new panel lays out into the same content row, so
    // that is the right target. Each axis falls back to `content`
    // when it measures 0 (a Collapsed panel before its deferred
    // activation).
    template <typename TPrimary, typename TFallback>
    static PhysicalSize ForNewTab(TPrimary const& active,
                                  TFallback const& content)
    {
        auto size = MeasuredPhysical(active);
        if (size.width == 0 || size.height == 0) {
            auto fallback = MeasuredPhysical(content);
            if (size.width == 0)  size.width  = fallback.width;
            if (size.height == 0) size.height = fallback.height;
        }
        return size;
    }
};

}  // namespace winrt::GhosttyWin32::implementation::display
