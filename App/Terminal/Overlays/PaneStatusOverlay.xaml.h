#pragma once

#if __has_include("Terminal/Overlays/PaneStatusOverlay.g.h")
#include "Terminal/Overlays/PaneStatusOverlay.g.h"
#else
#include "PaneStatusOverlay.g.h"
#endif
#include "ghostty.h"
#include <vector>

namespace winrt::GhosttyWin32::implementation
{
    // The pane's passive status overlays: hovered-link banner,
    // read-only / secure-input badges, and the key-state badge. Pure
    // display — it takes no input, owns no timers, and never talks
    // to ghostty; the TerminalControl composite forwards the
    // corresponding ISurfaceView calls here. The little state it
    // holds (secure-input flag, key-table stack, pending chord) is
    // here because the actions only carry deltas and this is where
    // they render. UI thread only.
    struct PaneStatusOverlay : PaneStatusOverlayT<PaneStatusOverlay>
    {
        PaneStatusOverlay();

        // MOUSE_OVER_LINK: show the banner with `url`, hide on empty.
        void SetHoveredLink(winrt::hstring const& url);

        // READONLY: indicator only — the write blocking is core-side.
        void SetReadonly(bool readonly);

        // SECURE_INPUT: ON / OFF set the state, TOGGLE flips it here
        // (ghostty's toggle keybind carries no absolute value).
        void SetSecureInput(ghostty_action_secure_input_e mode);

        // KEY_SEQUENCE / KEY_TABLE: pending chord labels and the
        // key-table name stack. The badge is rebuilt from both lists
        // on every change — they are tiny (a table stack is 1-2 deep,
        // a chord is 2-3 keys).
        void AppendKeySequence(winrt::hstring const& label);
        void ClearKeySequence();
        void PushKeyTable(winrt::hstring const& name);
        void PopKeyTable(bool all);

    private:
        void UpdateKeyStateBadge();

        bool m_secureInput = false;
        std::vector<winrt::hstring> m_keyTables;
        std::vector<winrt::hstring> m_keySequence;
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct PaneStatusOverlay : PaneStatusOverlayT<PaneStatusOverlay, implementation::PaneStatusOverlay>
    {
    };
}
