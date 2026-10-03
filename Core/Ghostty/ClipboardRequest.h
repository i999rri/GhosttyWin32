#pragma once

#include "ghostty.h"

namespace core::ghostty {

// The kind of a clipboard request libghostty asks the host to confirm,
// and what the host may answer without asking the user.
//
// libghostty asks when the config says `ask` for the request's kind
// (the default for `clipboard-read`) or when a paste looks unsafe, and
// expects the host to put the question to the user. This host has no
// such prompt yet, so it has to answer on the user's behalf.
class ClipboardRequest {
public:
    constexpr explicit ClipboardRequest(ghostty_clipboard_request_e raw) noexcept
        : m_raw(raw) {}

    // A paste the user started (Ctrl+V, the paste action). libghostty
    // asks about one only when the text trips paste protection.
    constexpr bool IsPaste() const noexcept {
        return m_raw == GHOSTTY_CLIPBOARD_REQUEST_PASTE;
    }

    // Any other request comes from whatever runs in the terminal (OSC
    // 52, the kitty clipboard protocol), possibly a remote host. A kind
    // this host does not know yet counts as one, so a request type that
    // libghostty adds later is refused until someone decides otherwise.
    constexpr bool IsFromTerminal() const noexcept { return !IsPaste(); }

    // Whether the host may confirm the request without a prompt: only
    // a paste. Refusing a paste would make every paste that trips paste
    // protection fail outright; confirming a terminal request would
    // hand it the clipboard with no prompt at all.
    // `clipboard-read = allow` still lets terminal reads through, since
    // libghostty then never asks.
    constexpr bool ConfirmsWithoutPrompt() const noexcept { return IsPaste(); }

private:
    ghostty_clipboard_request_e m_raw;
};

}  // namespace core::ghostty
