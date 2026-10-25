#pragma once

#include <optional>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.Foundation.h>

#include "ghostty.h"

namespace winrt::GhosttyWin32::implementation::input {

// A button going down or up, in libghostty's terms.
struct PointerButtonChange {
    ghostty_input_mouse_button_e button;
    ghostty_input_mouse_state_e state;
};

// One pointer event, read once. XAML hands every pointer event --
// pressed, released, moved -- the same PointerRoutedEventArgs, so
// the reading is the same too, and the handlers pass what they are
// given straight through. Peer of TerminalKeyDown: the WinUI args are
// taken apart here, and nothing past this point sees them.
//
// PointerUpdateKind is the one field that names the button an event
// changed. The IsLeftButtonPressed family reports every button still
// held, so at a release it says nothing, and with two held it cannot
// say which moved. Only the first mouse button reaches PointerPressed
// and PointerReleased; XAML ties one button to the pointer at a time
// and routes every other click through PointerMoved
// (UIElement.PointerPressed, Remarks), which is why a move can carry
// a change.
//
// A kind outside the table leaves `change` empty. That is every plain
// move, and it is also what a touch contact or a pen tip reports if
// Windows gives them no button -- the documentation does not say, and
// neither has been tried here. XBUTTON1 and XBUTTON2 have kinds of
// their own and libghostty has GHOSTTY_MOUSE_FOUR upwards for them;
// they wait for a mouse that has them.
struct PointerEvent {
    // Relative to the panel, which is what libghostty expects.
    winrt::Windows::Foundation::Point position;
    std::optional<PointerButtonChange> change;
    // What the panel captures and releases.
    winrt::Microsoft::UI::Xaml::Input::Pointer pointer;

    static PointerEvent from(
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args,
        winrt::Microsoft::UI::Xaml::UIElement const& panel)
    {
        auto point = args.GetCurrentPoint(panel);
        return PointerEvent{
            .position = point.Position(),
            .change = changeOf(point.Properties().PointerUpdateKind()),
            .pointer = args.Pointer(),
        };
    }

private:
    using PointerUpdateKind = winrt::Microsoft::UI::Input::PointerUpdateKind;

    struct Row {
        PointerUpdateKind kind;
        PointerButtonChange change;
    };

    static constexpr Row kTable[] = {
        { PointerUpdateKind::LeftButtonPressed,    { GHOSTTY_MOUSE_LEFT,   GHOSTTY_MOUSE_PRESS   } },
        { PointerUpdateKind::LeftButtonReleased,   { GHOSTTY_MOUSE_LEFT,   GHOSTTY_MOUSE_RELEASE } },
        { PointerUpdateKind::RightButtonPressed,   { GHOSTTY_MOUSE_RIGHT,  GHOSTTY_MOUSE_PRESS   } },
        { PointerUpdateKind::RightButtonReleased,  { GHOSTTY_MOUSE_RIGHT,  GHOSTTY_MOUSE_RELEASE } },
        { PointerUpdateKind::MiddleButtonPressed,  { GHOSTTY_MOUSE_MIDDLE, GHOSTTY_MOUSE_PRESS   } },
        { PointerUpdateKind::MiddleButtonReleased, { GHOSTTY_MOUSE_MIDDLE, GHOSTTY_MOUSE_RELEASE } },
    };

    static std::optional<PointerButtonChange> changeOf(PointerUpdateKind kind) noexcept
    {
        for (auto const& row : kTable) {
            if (row.kind == kind) return row.change;
        }
        return std::nullopt;
    }
};

}  // namespace winrt::GhosttyWin32::implementation::input
