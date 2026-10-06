#pragma once

#include "ghostty.h"
#include <cstdint>

namespace core::input {

// The mouse buttons this host has told libghostty are down.
//
// The keyboard needs no counterpart. ghostty answers a key event and
// keeps nothing the host has to stay in step with, but it does track
// mouse buttons, one state per button
// (external/ghostty/src/Surface.zig:226), and reports pointer motion
// as a drag of whichever one is held (:4657). A press the host never
// takes back turns every later move into a drag that is not
// happening.
//
// Windows has five buttons and they are independent, so the record
// is a bit per button rather than a single slot.
class PressedMouseButtons {
public:
    // True when this is the button's first press, which is the only
    // one libghostty should hear.
    bool Press(ghostty_input_mouse_button_e button) noexcept {
        uint32_t const bit = Bit(button);
        if ((m_down & bit) != 0) return false;

        m_down |= bit;
        return true;
    }

    // True when a press for this button was sent and is now taken
    // back. A release whose press never went out answers false: the
    // right-click that copies a selection sends none, and neither
    // does a press that lands before the surface exists.
    bool Release(ghostty_input_mouse_button_e button) noexcept {
        uint32_t const bit = Bit(button);
        if ((m_down & bit) == 0) return false;

        m_down &= ~bit;
        return true;
    }

private:
    static uint32_t Bit(ghostty_input_mouse_button_e button) noexcept {
        static_assert(GHOSTTY_MOUSE_ELEVEN < 32,
                      "one bit per button no longer fits the record");
        return uint32_t{ 1 } << static_cast<uint32_t>(button);
    }

    uint32_t m_down = 0;
};

}  // namespace core::input
