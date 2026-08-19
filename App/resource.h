#pragma once

// Win32 resource IDs embedded in GhosttyWin32.exe (see App.rc).
//
// IDC_GHOSTTY_BLANK_CURSOR: 32x32 fully-transparent cursor used by
// TerminalControl::SetMouseVisibility to hide the pointer while
// typing. WinUI 3 has no "hide" concept on ProtectedCursor — null
// means "inherit the parent's cursor" (renders as Arrow, verified),
// so hiding is expressed as showing a cursor with no pixels.
#define IDC_GHOSTTY_BLANK_CURSOR 101
