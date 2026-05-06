#pragma once

#include "TerminalControl.g.h"
#include "ghostty.h"

namespace winrt::GhosttyWin32::implementation
{
    // UserControl wrapper around a SwapChainPanel for one terminal surface.
    //
    // Why a UserControl? SwapChainPanel inherits from Grid (not Control)
    // and is not a default tab stop, so SwapChainPanel.Focus() returns
    // false in many normal contexts (the WinUI focus walker only finds
    // focusable descendants, of which there are none). Wrapping it in a
    // UserControl with IsTabStop=true gives us a real focus target —
    // Tab::Focus() can then call control.Focus(Programmatic) reliably,
    // matching the pattern Windows Terminal uses around its TermControl.
    //
    // Skeleton: holds the surface + composition handle pointers and
    // exposes the inner panel via the auto-generated x:Name accessor.
    // Input handlers (KeyDown / IME / Pointer) and resize handling will
    // move in here in subsequent commits — see the migration plan in
    // refactor/terminal-control branch history.
    struct TerminalControl : TerminalControlT<TerminalControl>
    {
        TerminalControl();

        // Implementation-only accessors. These can't go through IDL
        // because ghostty_surface_t / HANDLE don't have WinRT projections.
        // Callers reach them via winrt::get_self<implementation::TerminalControl>.
        ghostty_surface_t Surface() const noexcept { return m_surface; }
        HANDLE CompositionHandle() const noexcept { return m_compositionHandle; }
        void SetSurface(ghostty_surface_t s) noexcept { m_surface = s; }
        void SetCompositionHandle(HANDLE h) noexcept { m_compositionHandle = h; }

    private:
        ghostty_surface_t m_surface{ nullptr };
        HANDLE m_compositionHandle{ nullptr };
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct TerminalControl : TerminalControlT<TerminalControl, implementation::TerminalControl>
    {
    };
}
