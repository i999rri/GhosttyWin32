#include "pch.h"
#include "../Core/Ghostty/ClipboardConfirmPolicy.h"

using core::ghostty::ConfirmsWithoutPrompt;

TEST(ClipboardConfirmPolicyTest, ConfirmsAPasteTheUserStarted) {
    EXPECT_TRUE(ConfirmsWithoutPrompt(GHOSTTY_CLIPBOARD_REQUEST_PASTE));
}

TEST(ClipboardConfirmPolicyTest, RefusesRequestsFromTheTerminal) {
    // With the default `clipboard-read = ask`, confirming these would
    // hand the clipboard to whatever runs in the terminal unprompted.
    EXPECT_FALSE(ConfirmsWithoutPrompt(GHOSTTY_CLIPBOARD_REQUEST_OSC_52_READ));
    EXPECT_FALSE(ConfirmsWithoutPrompt(GHOSTTY_CLIPBOARD_REQUEST_OSC_52_WRITE));
    EXPECT_FALSE(ConfirmsWithoutPrompt(GHOSTTY_CLIPBOARD_REQUEST_KITTY_READ));
    EXPECT_FALSE(ConfirmsWithoutPrompt(GHOSTTY_CLIPBOARD_REQUEST_KITTY_WRITE));
    EXPECT_FALSE(ConfirmsWithoutPrompt(GHOSTTY_CLIPBOARD_REQUEST_LIST));
}
