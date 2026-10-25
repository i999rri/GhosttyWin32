#pragma once

#include "ghostty.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>

namespace core::input {

// The mouse buttons this host has told libghostty are down -- not
// the buttons Windows says are down, which every pointer event
// carries anyway. The two differ when a press is deliberately not
// sent, as for the right-click that copies a selection, and then this
// record is the one libghostty agrees with.
//
// It matters because libghostty keeps one state per button
// (external/ghostty/src/Surface.zig:226) and reports motion as a drag
// of whichever is held (:4657): a press never taken back turns every
// later move into a drag that is not happening.
class MouseButtonLedger {
public:
    // True when this is the button's first press, which is the only
    // one libghostty should hear.
    bool Press(ghostty_input_mouse_button_e button) noexcept {
        bool& relayed = Slot(button);
        if (relayed) return false;

        relayed = true;
        return true;
    }

    // True when a press for this button was sent and is now taken
    // back. A release whose press never went out answers false: the
    // right-click that copies a selection sends none, and neither
    // does a press that lands before the surface exists.
    bool Release(ghostty_input_mouse_button_e button) noexcept {
        bool& relayed = Slot(button);
        if (!relayed) return false;

        relayed = false;
        return true;
    }

    // True while this button's press is outstanding. The host asks
    // before it decides what a press is, and records it afterwards.
    bool IsOutstanding(ghostty_input_mouse_button_e button) const noexcept {
        return m_relayed[static_cast<size_t>(button)];
    }

    // True while any press is outstanding. The host holds the pointer
    // capture for exactly this long: it is what brings a release that
    // happens outside the panel back to this control, and keeping it
    // after the last button is up would take the pointer away from
    // everything else.
    bool AnyOutstanding() const noexcept {
        return std::any_of(m_relayed.begin(), m_relayed.end(),
                           [](bool relayed) { return relayed; });
    }

    // Takes back one of the presses still outstanding, until none is
    // left. The releases arrive one at a time everywhere except when
    // the capture is lost, where none of them is going to arrive at
    // all.
    std::optional<ghostty_input_mouse_button_e> ReleaseAny() noexcept {
        for (size_t i = 0; i < m_relayed.size(); ++i) {
            if (!m_relayed[i]) continue;

            m_relayed[i] = false;
            return static_cast<ghostty_input_mouse_button_e>(i);
        }
        return std::nullopt;
    }

private:
    // The enum's own value is the slot, which is what ghostty's
    // `Button.max` exists for on the Zig side
    // (external/ghostty/src/input/mouse.zig:34). The C header stops at
    // the last button instead of naming a count, so the size is
    // derived here.
    static constexpr size_t kButtonCount = GHOSTTY_MOUSE_ELEVEN + 1;

    bool& Slot(ghostty_input_mouse_button_e button) noexcept {
        return m_relayed[static_cast<size_t>(button)];
    }

    std::array<bool, kButtonCount> m_relayed{};
};

}  // namespace core::input
