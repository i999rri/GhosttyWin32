#pragma once

#include "ghostty.h"
#include <optional>

namespace core::input {

// A mouse button in this host's own words. Whoever holds one asks it
// which button it is; the comparison is made here, once, instead of
// wherever a button is read.
//
// The value is this class's own, not libghostty's, so Core's idea of
// a button does not depend on libghostty's numbering. The two meet
// only at ToGhosttyEnum and FromGhosttyEnum, which is where the
// translation lives.
class MouseButton {
public:
    static constexpr MouseButton Left()   noexcept { return MouseButton{ Value::Left }; }
    static constexpr MouseButton Right()  noexcept { return MouseButton{ Value::Right }; }
    static constexpr MouseButton Middle() noexcept { return MouseButton{ Value::Middle }; }

    constexpr bool IsLeft()   const noexcept { return m_value == Value::Left; }
    constexpr bool IsRight()  const noexcept { return m_value == Value::Right; }
    constexpr bool IsMiddle() const noexcept { return m_value == Value::Middle; }

    constexpr ghostty_input_mouse_button_e ToGhosttyEnum() const noexcept {
        switch (m_value) {
        case Value::Left:   return GHOSTTY_MOUSE_LEFT;
        case Value::Right:  return GHOSTTY_MOUSE_RIGHT;
        case Value::Middle: return GHOSTTY_MOUSE_MIDDLE;
        }
        return GHOSTTY_MOUSE_UNKNOWN;
    }

    // Empty for a button this host has no word for: libghostty's
    // fourth button and up, and its unknown.
    static constexpr std::optional<MouseButton> FromGhosttyEnum(
        ghostty_input_mouse_button_e button) noexcept {
        switch (button) {
        case GHOSTTY_MOUSE_LEFT:   return Left();
        case GHOSTTY_MOUSE_RIGHT:  return Right();
        case GHOSTTY_MOUSE_MIDDLE: return Middle();
        default:                   return std::nullopt;
        }
    }

    constexpr bool operator==(MouseButton const&) const noexcept = default;

private:
    enum class Value { Left, Right, Middle };

    explicit constexpr MouseButton(Value value) noexcept : m_value(value) {}

    Value m_value;
};

}  // namespace core::input
