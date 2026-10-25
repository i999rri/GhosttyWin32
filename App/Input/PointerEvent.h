#pragma once

#include <variant>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.Foundation.h>

#include "ghostty.h"

namespace winrt::GhosttyWin32::implementation::input {

// What one pointer event was, in libghostty's terms. XAML hands every
// pointer event -- pressed, released, moved -- the same
// PointerRoutedEventArgs, and nothing in the args says which event it
// came from; the one field that tells them apart is PointerUpdateKind,
// which names the button that changed. So the event is read once,
// here, into one of three shapes, and the handlers pass what they are
// given straight through. Peer of TerminalKeyDown: the WinUI args are
// taken apart here, and nothing past this point sees them.
//
// Positions are relative to the panel, which is what libghostty
// expects. The pointer is what the panel captures and releases.

struct PointerMove {
    winrt::Windows::Foundation::Point position;
};

struct PointerPress {
    winrt::Windows::Foundation::Point position;
    ghostty_input_mouse_button_e button;
    winrt::Microsoft::UI::Xaml::Input::Pointer pointer;
};

struct PointerRelease {
    winrt::Windows::Foundation::Point position;
    ghostty_input_mouse_button_e button;
    winrt::Microsoft::UI::Xaml::Input::Pointer pointer;
};

using PointerEvent = std::variant<PointerMove, PointerPress, PointerRelease>;

// The IsLeftButtonPressed family reports every button still held, so
// at a release it says nothing, and with two held it cannot say which
// moved; PointerUpdateKind can. Only the first mouse button reaches
// PointerPressed and PointerReleased -- XAML ties one button to the
// pointer at a time and routes every other click through PointerMoved
// (UIElement.PointerPressed, Remarks) -- which is why a move can turn
// out to be a press or a release.
//
// A kind the switch does not name is a move. That is every plain move,
// and it is also what a touch contact or a pen tip reports if Windows
// gives them no button; the documentation does not say, and neither
// has been tried here. XBUTTON1 and XBUTTON2 have kinds of their own
// and libghostty has GHOSTTY_MOUSE_FOUR upwards for them; they wait
// for a mouse that has them.
inline PointerEvent fromPointerArgs(
    winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args,
    winrt::Microsoft::UI::Xaml::UIElement const& panel)
{
    using winrt::Microsoft::UI::Input::PointerUpdateKind;

    auto point = args.GetCurrentPoint(panel);
    auto at = point.Position();
    auto press   = [&](ghostty_input_mouse_button_e b) { return PointerPress{ at, b, args.Pointer() }; };
    auto release = [&](ghostty_input_mouse_button_e b) { return PointerRelease{ at, b, args.Pointer() }; };

    switch (point.Properties().PointerUpdateKind()) {
    case PointerUpdateKind::LeftButtonPressed:    return press(GHOSTTY_MOUSE_LEFT);
    case PointerUpdateKind::LeftButtonReleased:   return release(GHOSTTY_MOUSE_LEFT);
    case PointerUpdateKind::RightButtonPressed:   return press(GHOSTTY_MOUSE_RIGHT);
    case PointerUpdateKind::RightButtonReleased:  return release(GHOSTTY_MOUSE_RIGHT);
    case PointerUpdateKind::MiddleButtonPressed:  return press(GHOSTTY_MOUSE_MIDDLE);
    case PointerUpdateKind::MiddleButtonReleased: return release(GHOSTTY_MOUSE_MIDDLE);
    default:                                      return PointerMove{ at };
    }
}

}  // namespace winrt::GhosttyWin32::implementation::input
