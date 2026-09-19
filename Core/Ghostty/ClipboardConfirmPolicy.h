#pragma once

#include "ghostty.h"

namespace core::ghostty {

// Whether a clipboard request that libghostty wants confirmed may be
// confirmed without asking. libghostty asks when the config says `ask`
// (the default for `clipboard-read`) or when a paste looks unsafe, and
// expects the host to put the question to the user. This host has no
// such prompt yet, so it has to answer on the user's behalf.
//
// Only a paste is confirmed. The user started it, and libghostty asks
// only because the text trips paste protection; refusing would make
// every such paste fail outright. Every other request comes from
// whatever runs in the terminal (OSC 52, kitty clipboard), possibly a
// remote host, so answering yes would hand it the clipboard with no
// prompt at all; those are refused. `clipboard-read = allow` still
// lets them through, since libghostty then never asks.
inline bool ConfirmsWithoutPrompt(ghostty_clipboard_request_e request) noexcept {
    return request == GHOSTTY_CLIPBOARD_REQUEST_PASTE;
}

}  // namespace core::ghostty
