#pragma once

#include "ghostty.h"
#include <windows.h>

namespace core::ghostty::actions::tags {

// SIZE_LIMIT action value (min/max window track size) plus the
// WM_GETMINMAXINFO subclass that enforces it. Named after the
// ghostty action tag so the relationship with action_cb is
// obvious; the class is "the SIZE_LIMIT thing", not "something
// that limits sizes".
//
// The subclass installs lazily on the first Apply so windows
// that never receive a SIZE_LIMIT don't pay the subclass cost.
// Win32 auto-removes subclasses when the HWND is destroyed, so
// no explicit teardown is needed.
class SizeLimit {
public:
    SizeLimit() = default;
    SizeLimit(const SizeLimit&) = delete;
    SizeLimit& operator=(const SizeLimit&) = delete;

    // Update the active limit and install the subclass on first
    // use. Zero in any limit field means "no constraint on this
    // axis" — only populated fields override the default min/max.
    void Apply(HWND hwnd, ghostty_action_size_limit_s limit) noexcept;

private:
    static LRESULT CALLBACK SubclassProc(
        HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
        UINT_PTR id, DWORD_PTR ref) noexcept;

    ghostty_action_size_limit_s m_value{};
    bool m_subclassed = false;
};

}  // namespace core::ghostty::actions::tags
