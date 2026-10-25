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
// A kind outside the table is a move. That is every plain move, and
// it is also what a touch contact or a pen tip reports if Windows
// gives them no button; the documentation does not say, and neither
// has been tried here. XBUTTON1 and XBUTTON2 have kinds of their own
// and libghostty has GHOSTTY_MOUSE_FOUR upwards for them; they wait
// for a mouse that has them.
inline PointerEvent fromPointerArgs(
    winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args,
    winrt::Microsoft::UI::Xaml::UIElement const& panel)
{
    using winrt::Microsoft::UI::Input::PointerUpdateKind;

    struct Row {
        PointerUpdateKind kind;
        ghostty_input_mouse_button_e button;
        ghostty_input_mouse_state_e state;
    };
    static constexpr Row kTable[] = {
        { PointerUpdateKind::LeftButtonPressed,    GHOSTTY_MOUSE_LEFT,   GHOSTTY_MOUSE_PRESS   },
        { PointerUpdateKind::LeftButtonReleased,   GHOSTTY_MOUSE_LEFT,   GHOSTTY_MOUSE_RELEASE },
        { PointerUpdateKind::RightButtonPressed,   GHOSTTY_MOUSE_RIGHT,  GHOSTTY_MOUSE_PRESS   },
        { PointerUpdateKind::RightButtonReleased,  GHOSTTY_MOUSE_RIGHT,  GHOSTTY_MOUSE_RELEASE },
        { PointerUpdateKind::MiddleButtonPressed,  GHOSTTY_MOUSE_MIDDLE, GHOSTTY_MOUSE_PRESS   },
        { PointerUpdateKind::MiddleButtonReleased, GHOSTTY_MOUSE_MIDDLE, GHOSTTY_MOUSE_RELEASE },
    };

    auto point = args.GetCurrentPoint(panel);
    auto position = point.Position();
    auto kind = point.Properties().PointerUpdateKind();

    for (auto const& row : kTable) {
        if (row.kind != kind) continue;

        if (row.state == GHOSTTY_MOUSE_PRESS) {
            return PointerPress{ position, row.button, args.Pointer() };
        }
        return PointerRelease{ position, row.button, args.Pointer() };
    }
    return PointerMove{ position };
}

}  // namespace winrt::GhosttyWin32::implementation::input
