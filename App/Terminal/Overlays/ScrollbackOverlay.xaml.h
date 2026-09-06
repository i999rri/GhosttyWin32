#pragma once

#include "ScrollbackOverlay.g.h"
#include "ghostty.h"
#include <cstdint>
#include <functional>

namespace winrt::GhosttyWin32::implementation
{
    // The pane's overlay scrollbar (#154). Owns the bar, its idle
    // fade timer and the echo guard; knows nothing about ghostty
    // beyond the SCROLLBAR payload shape. User intent leaves through
    // two callbacks the composite wires to the surface: an absolute
    // scroll-to-row from thumb drag / track click, and a wheel delta
    // for wheel events over the bar itself (its default wheel
    // handling would scroll by SmallChange and echo through
    // ValueChanged — correct but jerky). UI thread only.
    struct ScrollbackOverlay : ScrollbackOverlayT<ScrollbackOverlay>
    {
        ScrollbackOverlay();

        // Reflect the SCROLLBAR report: total scrollback rows,
        // viewport offset, viewport length. Collapses the bar when
        // nothing is scrollable, otherwise reveals it and (re)starts
        // the idle fade. Core-driven updates are guarded so the bar's
        // ValueChanged does not echo back into a scroll (the GTK
        // apprt blocks its adjustment signals the same way).
        void SetScrollbar(ghostty_action_scrollbar_s bar);

        // Stop the fade timer — called from the composite's Detach so
        // no tick lands during teardown (the timer holds only a weak
        // ref, this is belt and braces).
        void Stop();

        void SetOnScrollToRow(std::function<void(uint64_t)> cb) noexcept { m_onScrollToRow = std::move(cb); }
        void SetOnWheel(std::function<void(int)> cb) noexcept { m_onWheel = std::move(cb); }

    private:
        void Reveal();
        void FadeIfIdle();

        // m_syncing is set while SetScrollbar writes the bar's
        // properties so the resulting ValueChanged is recognised as
        // an echo. m_hovered keeps the bar visible while the pointer
        // is over it; the idle timer fades it otherwise.
        bool m_syncing = false;
        bool m_hovered = false;
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_fadeTimer{ nullptr };

        std::function<void(uint64_t)> m_onScrollToRow;
        std::function<void(int)> m_onWheel;
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct ScrollbackOverlay : ScrollbackOverlayT<ScrollbackOverlay, implementation::ScrollbackOverlay>
    {
    };
}
