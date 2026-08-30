#pragma once

#include "ghostty.h"

namespace core::ghostty::actions::tags {

// SIZE_LIMIT action value: the min / max window size ghostty asks
// for, and the one decision made from it — given the sizes the OS
// would otherwise allow, which to enforce. Zero in any field means
// "no constraint on this axis": only populated fields override.
//
// Pure value object. Enforcing the result on a window is
// win32::NativeWindow's job (WM_GETMINMAXINFO); this class never
// sees an HWND, so the override rule is unit-testable on its own.
class SizeLimit {
public:
    // Track sizes as WM_GETMINMAXINFO speaks of them: the smallest
    // and largest size the user can drag the window to.
    struct Track {
        long minWidth;
        long minHeight;
        long maxWidth;
        long maxHeight;
    };

    SizeLimit() = default;

    // Take a SIZE_LIMIT report.
    void Apply(ghostty_action_size_limit_s limit) noexcept { m_value = limit; }

    ghostty_action_size_limit_s const& Value() const noexcept { return m_value; }

    // The track sizes to enforce, given what the OS proposes.
    // Populated fields win; zero fields leave the proposal as is.
    Track Clamp(Track proposed) const noexcept {
        if (m_value.min_width)  proposed.minWidth  = static_cast<long>(m_value.min_width);
        if (m_value.min_height) proposed.minHeight = static_cast<long>(m_value.min_height);
        if (m_value.max_width)  proposed.maxWidth  = static_cast<long>(m_value.max_width);
        if (m_value.max_height) proposed.maxHeight = static_cast<long>(m_value.max_height);
        return proposed;
    }

private:
    ghostty_action_size_limit_s m_value{};
};

}  // namespace core::ghostty::actions::tags
