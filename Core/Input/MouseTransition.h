#pragma once

#include "Input/MouseButton.h"
#include "Input/MouseButtonLedger.h"
#include "ghostty.h"
#include <utility>
#include <vector>

namespace core::input {

// A press or release libghostty should hear.
struct ButtonReport {
    MouseButton button;
    ghostty_input_mouse_state_e state;

    constexpr bool operator==(ButtonReport const&) const noexcept = default;
};

// What one mouse event does to the host, as a value: the ledger as it
// is afterwards, and what goes out to the panel and the terminal.
// Built by the functions below from the ledger as it was, so the
// deciding is pure and tested, and the owner only applies it.
struct MouseTransition {
    // What happens to the pointer capture: nothing, taken on this
    // event, or let go. Asked rather than compared, as MouseButton is.
    class Capture {
    public:
        static constexpr Capture Keep() noexcept { return Capture{ Value::Keep }; }
        static constexpr Capture Take() noexcept { return Capture{ Value::Take }; }
        static constexpr Capture Drop() noexcept { return Capture{ Value::Drop }; }

        constexpr bool IsKeep() const noexcept { return m_value == Value::Keep; }
        constexpr bool IsTake() const noexcept { return m_value == Value::Take; }
        constexpr bool IsDrop() const noexcept { return m_value == Value::Drop; }

        constexpr bool operator==(Capture const&) const noexcept = default;

    private:
        enum class Value { Keep, Take, Drop };
        explicit constexpr Capture(Value value) noexcept : m_value(value) {}
        Value m_value;
    };

    MouseButtonLedger ledger;
    Capture capture = Capture::Keep();
    std::vector<ButtonReport> reports;
    bool copySelection = false;
};

// A button went down. The first press takes the pointer capture, so a
// release outside the panel still arrives; a press libghostty has
// already heard changes nothing. A right button pressed on its own,
// over a selection, is this terminal's copy gesture: libghostty hears
// no press, and so hears no release either. Under a held left one the
// right button reaches the terminal, because copying would clear the
// selection the left one is drawing.
inline MouseTransition OnPress(MouseButtonLedger ledger, MouseButton button, bool hasSelection)
{
    MouseTransition t{ .ledger = ledger };
    if (ledger.IsOutstanding(button.ToGhosttyEnum())) return t;

    bool const pressedAlone = !ledger.AnyOutstanding();
    bool const isCopyGesture = button.IsRight() && pressedAlone && hasSelection;
    if (isCopyGesture) {
        t.copySelection = true;
        return t;
    }

    t.ledger.Press(button.ToGhosttyEnum());
    if (pressedAlone) t.capture = MouseTransition::Capture::Take();
    t.reports.push_back({ button, GHOSTTY_MOUSE_PRESS });
    return t;
}

// A button came up. Only a press that went out is taken back; the
// capture is dropped once nothing is outstanding, whether or not this
// release was one of ours.
inline MouseTransition OnRelease(MouseButtonLedger ledger, MouseButton button)
{
    MouseTransition t{ .ledger = ledger };
    if (t.ledger.Release(button.ToGhosttyEnum())) {
        t.reports.push_back({ button, GHOSTTY_MOUSE_RELEASE });
    }
    if (!t.ledger.AnyOutstanding()) t.capture = MouseTransition::Capture::Drop();
    return t;
}

// The capture is gone, so the releases it was going to deliver are
// not coming: every outstanding press is taken back now, before
// libghostty reads the next move as a drag of a button nobody holds.
inline MouseTransition OnCaptureLost(MouseButtonLedger ledger)
{
    MouseTransition t{ .ledger = ledger };
    while (auto button = t.ledger.ReleaseAny()) {
        if (auto named = MouseButton::FromGhosttyEnum(*button)) {
            t.reports.push_back({ *named, GHOSTTY_MOUSE_RELEASE });
        }
    }
    return t;
}

// A canceled pointer ends its presses with no release at all, and the
// capture it held goes with them.
inline MouseTransition OnCanceled(MouseButtonLedger ledger)
{
    MouseTransition t = OnCaptureLost(std::move(ledger));
    t.capture = MouseTransition::Capture::Drop();
    return t;
}

}  // namespace core::input
