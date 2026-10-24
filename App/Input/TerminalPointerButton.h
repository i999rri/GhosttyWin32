#pragma once

#include <optional>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include "ghostty.h"

namespace winrt::GhosttyWin32::implementation::input {

// Domain model for the button of a pointer event arriving at
// TerminalControl. Peer of TerminalKeyDown and TerminalKeyUp: it
// says what the event names, and leaves what to do about it to
// core::input.
//
// PointerUpdateKind is the only field that names the one button this
// event changed. The IsLeftButtonPressed family reports every button
// still held, so with two of them down it cannot say which moved --
// a press of the second looks exactly like another press of the
// first, and a release looks like nothing at all.
//
// Microsoft documents PointerUpdateKind for the mouse buttons only
// ("Other: pointer updates not identified by other
// PointerUpdateKind values"), and a touch contact or a pen tip has
// no button of its own. Windows reports those as the primary
// action, which this terminal has always taken as the left button,
// so Other is read the same way -- but only for a press or a release
// event, because a plain move is Other too.
//
// Only the first mouse button reaches PointerPressed and
// PointerReleased. XAML ties one button to the pointer at a time,
// and "other mouse button clicks are routed through the PointerMoved
// event" (UIElement.PointerPressed), so a move can carry a press or
// a release of its own.
class TerminalPointerButton {
public:
    explicit TerminalPointerButton(
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) noexcept
        // The update kind describes the event, not where it landed,
        // so it needs no element to be relative to.
        : m_kind(args.GetCurrentPoint(nullptr).Properties().PointerUpdateKind())
    {}

    // The button a PointerPressed is about.
    std::optional<ghostty_input_mouse_button_e> pressedButton() const noexcept {
        if (auto named = namedChange(GHOSTTY_MOUSE_PRESS)) return named;
        return primaryContact();
    }

    // The button a PointerReleased is about.
    std::optional<ghostty_input_mouse_button_e> releasedButton() const noexcept {
        if (auto named = namedChange(GHOSTTY_MOUSE_RELEASE)) return named;
        return primaryContact();
    }

    // The button a PointerMoved pressed or released, if it carried
    // one. Only a kind in the table counts here: read as a contact,
    // every plain move would become a press.
    std::optional<ghostty_input_mouse_button_e> buttonPressedDuringMove() const noexcept {
        return namedChange(GHOSTTY_MOUSE_PRESS);
    }
    std::optional<ghostty_input_mouse_button_e> buttonReleasedDuringMove() const noexcept {
        return namedChange(GHOSTTY_MOUSE_RELEASE);
    }

private:
    using PointerUpdateKind = winrt::Microsoft::UI::Input::PointerUpdateKind;

    // One row of the translation: a kind Windows reports, and what it
    // means to libghostty.
    struct Change {
        PointerUpdateKind kind;
        ghostty_input_mouse_button_e button;
        ghostty_input_mouse_state_e state;
    };

    // Every kind this terminal passes on. XBUTTON1 and XBUTTON2 have
    // kinds of their own and libghostty has GHOSTTY_MOUSE_FOUR upwards
    // for them, but they are left out until they have been tried on a
    // mouse that has them.
    static constexpr Change kChanges[] = {
        { PointerUpdateKind::LeftButtonPressed,    GHOSTTY_MOUSE_LEFT,   GHOSTTY_MOUSE_PRESS   },
        { PointerUpdateKind::LeftButtonReleased,   GHOSTTY_MOUSE_LEFT,   GHOSTTY_MOUSE_RELEASE },
        { PointerUpdateKind::RightButtonPressed,   GHOSTTY_MOUSE_RIGHT,  GHOSTTY_MOUSE_PRESS   },
        { PointerUpdateKind::RightButtonReleased,  GHOSTTY_MOUSE_RIGHT,  GHOSTTY_MOUSE_RELEASE },
        { PointerUpdateKind::MiddleButtonPressed,  GHOSTTY_MOUSE_MIDDLE, GHOSTTY_MOUSE_PRESS   },
        { PointerUpdateKind::MiddleButtonReleased, GHOSTTY_MOUSE_MIDDLE, GHOSTTY_MOUSE_RELEASE },
    };

    // The button this event's kind names going `state`, if the table
    // has it.
    std::optional<ghostty_input_mouse_button_e> namedChange(
        ghostty_input_mouse_state_e state) const noexcept {
        for (auto const& change : kChanges) {
            if (change.kind == m_kind && change.state == state) return change.button;
        }
        return std::nullopt;
    }

    // A press or release event whose kind names no button: a touch
    // contact or a pen tip, taken as the left button.
    std::optional<ghostty_input_mouse_button_e> primaryContact() const noexcept {
        if (m_kind == PointerUpdateKind::Other) return GHOSTTY_MOUSE_LEFT;
        return std::nullopt;
    }

    PointerUpdateKind m_kind;
};

}  // namespace winrt::GhosttyWin32::implementation::input
