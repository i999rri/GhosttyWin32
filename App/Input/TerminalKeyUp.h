#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include "Input/KeyEventTranslator.h"

namespace winrt::GhosttyWin32::implementation::input {

// Domain model for a KeyUp event arriving at TerminalControl.
// Symmetric with TerminalKeyDown.
//
// Release events don't currently drive any host-side shortcut or
// IME logic of their own — the keystroke is forwarded straight to
// ghostty so binding triggers that fire on release can resolve.
// The class exists so the host can talk about "the key the user
// just released" instead of poking args.Key() directly, and so the
// translator pipeline keeps a uniform shape
// (TerminalKey* -> toRaw -> Translate -> ghostty_surface_key) for
// the press and release sides.
class TerminalKeyUp {
public:
    explicit TerminalKeyUp(
        winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) noexcept
        : m_args(args)
        , m_ctrl((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        , m_shift((GetKeyState(VK_SHIFT)   & 0x8000) != 0)
        , m_alt((GetKeyState(VK_MENU)      & 0x8000) != 0)
    {}

    core::input::RawKeyRelease toRawKeyRelease() const noexcept {
        return core::input::RawKeyRelease{
            .vk_code     = static_cast<uint32_t>(vk()),
            .scan_code   = static_cast<uint32_t>(m_args.KeyStatus().ScanCode),
            .is_extended = m_args.KeyStatus().IsExtendedKey,
            .shift       = m_shift,
            .ctrl        = m_ctrl,
            .alt         = m_alt,
        };
    }

    int vk() const noexcept { return static_cast<int>(m_args.Key()); }
    bool ctrl() const noexcept { return m_ctrl; }
    bool shift() const noexcept { return m_shift; }
    bool alt() const noexcept { return m_alt; }

private:
    winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs m_args;
    bool m_ctrl;
    bool m_shift;
    bool m_alt;
};

}  // namespace winrt::GhosttyWin32::implementation::input
