#include "pch.h"
#include "../Core/Ghostty/ClipboardRequest.h"

using core::ghostty::ClipboardRequest;

namespace {

// Every request type the terminal can send.
constexpr ghostty_clipboard_request_e kTerminalKinds[] = {
    GHOSTTY_CLIPBOARD_REQUEST_OSC_52_READ,
    GHOSTTY_CLIPBOARD_REQUEST_OSC_52_WRITE,
    GHOSTTY_CLIPBOARD_REQUEST_KITTY_READ,
    GHOSTTY_CLIPBOARD_REQUEST_KITTY_WRITE,
    GHOSTTY_CLIPBOARD_REQUEST_LIST,
};

}  // namespace

TEST(ClipboardRequestTest, APasteIsTheUsersAndIsConfirmed) {
    ClipboardRequest paste{ GHOSTTY_CLIPBOARD_REQUEST_PASTE };
    EXPECT_TRUE(paste.IsPaste());
    EXPECT_FALSE(paste.IsFromTerminal());
    EXPECT_TRUE(paste.ConfirmsWithoutPrompt());
}

TEST(ClipboardRequestTest, TerminalRequestsAreRefused) {
    // With the default `clipboard-read = ask`, confirming these would
    // hand the clipboard to whatever runs in the terminal unprompted.
    for (auto kind : kTerminalKinds) {
        ClipboardRequest request{ kind };
        EXPECT_FALSE(request.IsPaste()) << kind;
        EXPECT_TRUE(request.IsFromTerminal()) << kind;
        EXPECT_FALSE(request.ConfirmsWithoutPrompt()) << kind;
    }
}

TEST(ClipboardRequestTest, AnUnknownKindIsTreatedAsTheTerminals) {
    // A request type libghostty adds later must not be confirmed by
    // default; only the known paste is.
    ClipboardRequest unknown{ static_cast<ghostty_clipboard_request_e>(0x7fff) };
    EXPECT_FALSE(unknown.IsPaste());
    EXPECT_TRUE(unknown.IsFromTerminal());
    EXPECT_FALSE(unknown.ConfirmsWithoutPrompt());
}
