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

    // The button a PointerPressed is about.
    std::optional<ghostty_input_mouse_button_e> pressedButton() const noexcept {
        if (auto named = namedPress()) return named;
        return primaryContact();
    }

    // The button a PointerReleased is about.
    std::optional<ghostty_input_mouse_button_e> releasedButton() const noexcept {
        if (auto named = namedRelease()) return named;
        return primaryContact();
    }

    // The button a PointerMoved pressed or released, if it carried
    // one. Only a kind that names a button counts here: read as a
    // contact, every plain move would become a press.
    std::optional<ghostty_input_mouse_button_e> buttonPressedDuringMove() const noexcept {
        return namedPress();
    }
    std::optional<ghostty_input_mouse_button_e> buttonReleasedDuringMove() const noexcept {
        return namedRelease();
    }

private:
    std::optional<ghostty_input_mouse_button_e> namedPress() const noexcept {
        using winrt::Microsoft::UI::Input::PointerUpdateKind;
        switch (m_kind) {
        case PointerUpdateKind::LeftButtonPressed:   return GHOSTTY_MOUSE_LEFT;
        case PointerUpdateKind::RightButtonPressed:  return GHOSTTY_MOUSE_RIGHT;
        case PointerUpdateKind::MiddleButtonPressed: return GHOSTTY_MOUSE_MIDDLE;
        default:                                     return std::nullopt;
        }
    }

    std::optional<ghostty_input_mouse_button_e> namedRelease() const noexcept {
        using winrt::Microsoft::UI::Input::PointerUpdateKind;
        switch (m_kind) {
        case PointerUpdateKind::LeftButtonReleased:   return GHOSTTY_MOUSE_LEFT;
        case PointerUpdateKind::RightButtonReleased:  return GHOSTTY_MOUSE_RIGHT;
        case PointerUpdateKind::MiddleButtonReleased: return GHOSTTY_MOUSE_MIDDLE;
        default:                                      return std::nullopt;
        }
    }

    // A press or release event whose kind names no button: a touch
    // contact or a pen tip, taken as the left button.
    std::optional<ghostty_input_mouse_button_e> primaryContact() const noexcept {
        if (m_kind == winrt::Microsoft::UI::Input::PointerUpdateKind::Other) {
            return GHOSTTY_MOUSE_LEFT;
        }
        return std::nullopt;
    }

    winrt::Microsoft::UI::Input::PointerUpdateKind m_kind;
};

}  // namespace winrt::GhosttyWin32::implementation::input
