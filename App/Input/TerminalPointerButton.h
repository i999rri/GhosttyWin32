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
// so Other is read the same way here.
//
// Buttons four and up are left alone. Windows carries them as
// XBUTTON1 and XBUTTON2 and libghostty has GHOSTTY_MOUSE_FOUR
// upwards, but nothing here has been tried against a mouse that
// has them.
class TerminalPointerButton {
public:
    explicit TerminalPointerButton(
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) noexcept
        // The update kind describes the event, not where it landed,
        // so it needs no element to be relative to.
        : m_kind(args.GetCurrentPoint(nullptr).Properties().PointerUpdateKind())
    {}

    std::optional<ghostty_input_mouse_button_e> pressedButton() const noexcept {
        using winrt::Microsoft::UI::Input::PointerUpdateKind;
        switch (m_kind) {
        case PointerUpdateKind::LeftButtonPressed:
        case PointerUpdateKind::Other:
            return GHOSTTY_MOUSE_LEFT;
        case PointerUpdateKind::RightButtonPressed:
            return GHOSTTY_MOUSE_RIGHT;
        case PointerUpdateKind::MiddleButtonPressed:
            return GHOSTTY_MOUSE_MIDDLE;
        default:
            return std::nullopt;
        }
    }

    std::optional<ghostty_input_mouse_button_e> releasedButton() const noexcept {
        using winrt::Microsoft::UI::Input::PointerUpdateKind;
        switch (m_kind) {
        case PointerUpdateKind::LeftButtonReleased:
        case PointerUpdateKind::Other:
            return GHOSTTY_MOUSE_LEFT;
        case PointerUpdateKind::RightButtonReleased:
            return GHOSTTY_MOUSE_RIGHT;
        case PointerUpdateKind::MiddleButtonReleased:
            return GHOSTTY_MOUSE_MIDDLE;
        default:
            return std::nullopt;
        }
    }

private:
    winrt::Microsoft::UI::Input::PointerUpdateKind m_kind;
};

}  // namespace winrt::GhosttyWin32::implementation::input
