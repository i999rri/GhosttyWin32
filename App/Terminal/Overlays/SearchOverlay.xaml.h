#pragma once

#if __has_include("Terminal/Overlays/SearchOverlay.g.h")
#include "Terminal/Overlays/SearchOverlay.g.h"
#else
#include "SearchOverlay.g.h"
#endif
#include <cstddef>
#include <functional>

namespace winrt::GhosttyWin32::implementation
{
    // The pane's search bar. Owns the input box, the "n / total"
    // readout, the keystroke debounce and the open/closed state;
    // knows nothing about ghostty. User intent leaves through
    // callbacks the composite wires to the surface's binding actions
    // (search:, navigate_search:, end_search). UI thread only.
    //
    // Debounce mirrors the macOS SurfaceView: needles under 3 chars
    // wait 300ms (cheap keystrokes, expensive short-needle scans);
    // 3+ chars and empty go immediately.
    struct SearchOverlay : SearchOverlayT<SearchOverlay>
    {
        SearchOverlay();

        // START_SEARCH: show the bar, focus the box, select all. A
        // non-empty needle came from search_selection — ghostty has
        // already started that search, so it is pre-filled without
        // echoing back. An empty needle (bare start_search) leaves
        // the box as-is: re-opening keeps the last query, like every
        // editor's find bar.
        void Open(winrt::hstring const& needle);

        // END_SEARCH: hide the bar and cancel a pending debounce.
        // Returns whether it was open, so the composite hands focus
        // back to the terminal only in that case — ghostty can
        // END_SEARCH while focus is elsewhere (another pane), and
        // stealing it then would be a surprise.
        bool Close();

        bool IsOpen() const noexcept { return m_open; }

        // Whether the input box currently holds keyboard focus (false
        // while closed). This — not "open" — is what gates terminal
        // input: the bar can stay open while the user clicks back
        // into the terminal to keep typing (#171 review).
        bool BoxHasFocus();

        // If open, put keyboard focus on the box and return true.
        bool FocusInput();

        // SEARCH_TOTAL / SEARCH_SELECTED: -1 while unknown; selected
        // is 1-based.
        void SetTotal(ptrdiff_t total);
        void SetSelected(ptrdiff_t selected);

        // Stop the debounce timer — composite Detach.
        void Stop();

        // Needle to search for (already debounced); empty clears.
        void SetOnNeedle(std::function<void(winrt::hstring const&)> cb) noexcept { m_onNeedle = std::move(cb); }
        // Enter / Next button = true, Shift+Enter / Prev = false.
        void SetOnNavigate(std::function<void(bool)> cb) noexcept { m_onNavigate = std::move(cb); }
        // Esc / Close button. The composite asks ghostty to end the
        // search; the resulting END_SEARCH performs the actual hide,
        // so core and host never disagree about whether a search is
        // on.
        void SetOnCloseRequested(std::function<void()> cb) noexcept { m_onCloseRequested = std::move(cb); }

    private:
        void SendNeedle();
        void UpdateCount();

        // m_syncing guards the programmatic pre-fill in Open so
        // TextChanged doesn't fire a redundant search for text
        // ghostty just handed us.
        bool m_open = false;
        bool m_syncing = false;
        ptrdiff_t m_total = -1;
        ptrdiff_t m_selected = -1;
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_debounce{ nullptr };

        std::function<void(winrt::hstring const&)> m_onNeedle;
        std::function<void(bool)> m_onNavigate;
        std::function<void()> m_onCloseRequested;
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct SearchOverlay : SearchOverlayT<SearchOverlay, implementation::SearchOverlay>
    {
    };
}
