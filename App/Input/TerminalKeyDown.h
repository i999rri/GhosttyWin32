#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include "Input/KeyEventTranslator.h"

namespace winrt::GhosttyWin32::implementation::input {

// Domain model for a KeyDown event arriving at TerminalControl.
//
// The class answers two kinds of questions:
//
//   1. "What did the user mean by this keystroke?" — isImeKeystroke,
//      isCopyShortcut, isPasteShortcut. The caller reads its own
//      logic as prose ("if it's a copy shortcut and there's a
//      selection, copy the selection") instead of bit-fiddling
//      ctrl/shift/vk at every use site.
//
//   2. "What does ghostty need from it?" — toRawKeyPress. The
//      OS-specific assembly (ToUnicode dance, scan-code + extended
//      bit, modifier cache) lives here so the translator stays a
//      pure (RawKeyPress -> ghostty_input_key_s) function with no
//      Win32 / WinUI dependencies of its own.
//
// Construction reads the side-effecting OS state (GetKeyState
// modifier mask, ImeBuffer.composing()) once and caches it. Every
// accessor afterwards is a pure read of the snapshot, so the
// shortcut tests and the toRawKeyPress assembly see the same
// modifier state the isImeKeystroke check was tested against.
class TerminalKeyDown {
public:
    TerminalKeyDown(
        winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args,
        bool ime_composing) noexcept
        : m_args(args)
        , m_imeComposing(ime_composing)
        , m_ctrl((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        , m_shift((GetKeyState(VK_SHIFT)   & 0x8000) != 0)
        , m_alt((GetKeyState(VK_MENU)      & 0x8000) != 0)
    {}

    // ----- domain queries -----
    //
    // isImeKeystroke: the IME owns the lifecycle (composing in
    //   flight, or VK_PROCESSKEY signalling "the IME is about to
    //   eat this"). The EditContext handlers will react; the
    //   terminal must not double-encode.
    // isCopyShortcut: Ctrl+C with no extra modifiers. The host
    //   intercepts to copy the selection to the OS clipboard, but
    //   only if a selection actually exists — the call site has to
    //   check ghostty_surface_has_selection itself. Ctrl+C with no
    //   selection falls through to ghostty so the SIGINT path runs.
    // isPasteShortcut: Ctrl+V with no extra modifiers. The host
    //   reads the clipboard and feeds it via the paste API
    //   (ghostty_surface_text), bypassing the key-event pipeline.
    bool isImeKeystroke() const noexcept {
        return vk() == VK_PROCESSKEY || m_imeComposing;
    }
    bool isCopyShortcut() const noexcept {
        return m_ctrl && !m_shift && vk() == 'C';
    }
    bool isPasteShortcut() const noexcept {
        return m_ctrl && !m_shift && vk() == 'V';
    }

    // ----- convert to translator input -----
    //
    // text_buf owns the OS-translated UTF-8 the resulting
    // RawKeyPress points at, so it must outlive the returned struct.
    // The buffer should be at least a few bytes — 16 is comfortable
    // for any single codepoint plus its UTF-8 expansion.
    //
    // Two ToUnicode calls run inside:
    //
    //   * One with an empty keyboard state -> unshifted codepoint
    //     for ghostty's unicode-keyed bindings (`ctrl+shift+t = new_tab`
    //     matches 't', not the physical-key enum).
    //   * One with the live keyboard state -> the printable UTF-8
    //     the keystroke actually produced, populated into `text`
    //     only for codepoints >= 0x20 (control characters get
    //     re-encoded by ghostty itself from the scan code).
    //
    // Each call drains its dead-key state with VK_SPACE afterwards
    // so a real keystroke that lands next isn't affected.
    core::input::RawKeyPress toRawKeyPress(
        char* text_buf, std::size_t text_buf_size) const noexcept;

    // ----- raw accessors -----
    int vk() const noexcept { return static_cast<int>(m_args.Key()); }
    bool ctrl() const noexcept { return m_ctrl; }
    bool shift() const noexcept { return m_shift; }
    bool alt() const noexcept { return m_alt; }

private:
    winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs m_args;
    bool m_imeComposing;
    bool m_ctrl;
    bool m_shift;
    bool m_alt;
};

inline core::input::RawKeyPress TerminalKeyDown::toRawKeyPress(
    char* text_buf, std::size_t text_buf_size) const noexcept
{
    auto vk_code   = static_cast<uint32_t>(vk());
    auto scan_code = static_cast<uint32_t>(m_args.KeyStatus().ScanCode);

    BYTE plain_state[256] = {};
    wchar_t unshifted_chars[4] = {};
    int unshifted_count = ToUnicode(
        vk_code, scan_code, plain_state, unshifted_chars, 4, 0);
    wchar_t drain1[4] = {};
    ToUnicode(VK_SPACE, 0x39, plain_state, drain1, 4, 0);

    BYTE live_state[256] = {};
    GetKeyboardState(live_state);
    wchar_t live_chars[4] = {};
    int live_count = ToUnicode(
        vk_code, scan_code, live_state, live_chars, 4, 0);
    wchar_t drain2[4] = {};
    ToUnicode(VK_SPACE, 0x39, live_state, drain2, 4, 0);

    const char* text_ptr = nullptr;
    if (live_count > 0
        && live_chars[0] >= 0x20
        && text_buf
        && text_buf_size > 1)
    {
        int len = WideCharToMultiByte(
            CP_UTF8, 0, live_chars, live_count,
            text_buf, static_cast<int>(text_buf_size - 1),
            nullptr, nullptr);
        if (len > 0) {
            text_buf[len] = '\0';
            text_ptr = text_buf;
        }
    }

    return core::input::RawKeyPress{
        .vk_code     = vk_code,
        .scan_code   = scan_code,
        .is_extended = m_args.KeyStatus().IsExtendedKey,
        .shift       = m_shift,
        .ctrl        = m_ctrl,
        .alt         = m_alt,
        .composing   = false,  // isImeKeystroke already short-circuited the caller
        .text        = text_ptr,
        .unshifted_codepoint =
            (unshifted_count > 0 && unshifted_chars[0] >= 0x20)
                ? static_cast<uint32_t>(unshifted_chars[0]) : 0u,
    };
}

}  // namespace winrt::GhosttyWin32::implementation::input
