#pragma once

namespace winrt::GhosttyWin32::implementation
{
    // An overlay that can take the keyboard away from the terminal.
    // TerminalControl keeps the list of these; SurfaceHost's input
    // gate is answered from it (the terminal owns input only while no
    // overlay holds the keyboard). Implementing this is the whole
    // contract — every guard (keys, IME engagement, IME commits) is
    // derived from the answer, so a new overlay never writes one.
    struct IKeyboardOverlay
    {
        virtual ~IKeyboardOverlay() = default;

        // True while this overlay's input element has keyboard focus.
        // Focus, not "open": an open overlay whose box the user has
        // clicked away from must hand the keyboard back (#171 review).
        virtual bool HoldsKeyboardFocus() = 0;
    };
}
