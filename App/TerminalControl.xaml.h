#pragma once

#include "TerminalControl.g.h"
#include "SurfaceHost.h"
#include "Ghostty/Surface.h"
#include "Host/ISurfaceView.h"
#include "Interop/Encoding.h"
#include "ghostty.h"
#include <functional>
#include <memory>
#include <vector>

namespace winrt::GhosttyWin32::implementation
{
    // The pane's composite: one UserControl whose Grid stacks the
    // SwapChainPanel a SurfaceHost renders into with the in-tree
    // overlays layered on top (unfocused dim, opaque underlay, link
    // banner, status badges, scrollbar, search bar).
    //
    // SwapChainPanel inherits from Grid (not Control) and is not a
    // default tab stop, so SwapChainPanel.Focus() returns false in many
    // normal contexts. Wrapping it in a UserControl with IsTabStop=true
    // gives Tab::Focus() a target that programmatic focus moves
    // actually stick to — same pattern Windows Terminal uses around
    // its TermControl.
    //
    // Responsibilities kept here, because they need the composite:
    //   * XAML focus (Focus() on pointer press, GotFocus / LostFocus →
    //     SurfaceHost) and the "who owns text input" gate the host
    //     consults (the search box is a sibling the host cannot see).
    //   * ProtectedCursor — one writer composing ghostty's shape,
    //     the hidden state, and overlay hover.
    //   * core::host::ISurfaceView: libghostty's surface-targeted
    //     actions reach this control directly through the window's
    //     FindSurfaceView directory and are routed to the host or the
    //     overlay that renders them.
    // Surface ownership and lifetime, swap chain binding, input
    // translation and IME live on SurfaceHost; the public Attach /
    // Detach / Rehost / Surface() here forward to it so Tab,
    // TabFactory and MainWindow keep one handle per pane.
    struct TerminalControl : TerminalControlT<TerminalControl>, host::ISurfaceView
    {
        TerminalControl();
        ~TerminalControl();

        // Re-exposes the x:Name accessor publicly so external code (Tab,
        // TabFactory, MainWindow's pointer handlers) can reach the inner
        // SwapChainPanel without going through the impl class's name
        // resolution rules. Implementation-only — not in IDL.
        //
        // Not const: the auto-generated Panel() x:Name accessor on the
        // T<TerminalControl> base is non-const, so calling it from a
        // const method fails to convert `this`.
        Microsoft::UI::Xaml::Controls::SwapChainPanel InnerPanel() { return Panel(); }

        // ----- SurfaceHost forwarders (see SurfaceHost.h) -----
        void Attach(ghostty_app_t app,
                    ghostty_surface_t surface,
                    HANDLE compositionHandle,
                    HWND hostHwnd,
                    std::shared_ptr<SwapChainAttachRequest> attachRequest,
                    std::shared_ptr<SwapChainChangedContext> swapChainChangedContext) {
            m_host->Attach(app, surface, compositionHandle, hostHwnd,
                           std::move(attachRequest), std::move(swapChainChangedContext));
        }
        // Tear-down counterpart of Attach. Stops the overlay timers
        // first so no tick lands while the surface goes away.
        // Idempotent — calling twice (e.g. once from Tab::~Tab and
        // once from ~TerminalControl) is safe.
        void Detach();
        void Rehost(HWND hostHwnd,
                    std::function<void(ghostty_surface_t)> onFocused) noexcept {
            m_host->Rehost(hostHwnd, std::move(onFocused));
        }
        // Forwarded by MainWindow's window-Activated handler. XAML's
        // GotFocus / LostFocus don't fire on window de/activation
        // (focus is logically retained on the focused element across
        // alt-tab), so the host has to ping the active control whenever
        // the window crosses the activation boundary.
        void NotifyImeFocusEnter() { m_host->NotifyImeFocusEnter(); }
        void NotifyImeFocusLeave() { m_host->NotifyImeFocusLeave(); }
        void SetOnFocused(std::function<void(ghostty_surface_t)> cb) noexcept {
            m_host->SetOnFocused(std::move(cb));
        }

        // Surface() returns the wrapper so call sites can issue typed
        // operations (Refresh, Key, MouseButton, …) without touching
        // raw ghostty C API. Identity-comparison call sites (action
        // callbacks, FindPaneBySurface) use `Surface().Owns(handle)`.
        core::ghostty::Surface const& Surface() const noexcept { return m_host->Surface(); }
        core::ghostty::Surface& Surface() noexcept { return m_host->Surface(); }
        HANDLE CompositionHandle() const noexcept { return m_host->CompositionHandle(); }

        // ----- ISurfaceView -----

        // Apply a ghostty-requested mouse cursor shape. UI thread
        // only. Unrecognised shapes fall back to Arrow. Used both for
        // the initial IBeam set in the ctor and for live updates as
        // the pointer moves over/off cells, links, resize borders.
        void SetCursorShape(ghostty_action_mouse_shape_e shape) override;

        // Mirror ghostty's MOUSE_VISIBILITY on this pane: false hides
        // the pointer (mouse-hide-while-typing), true restores the
        // last MOUSE_SHAPE. ghostty drives both directions (hide on
        // keypress, show on pointer move), so this holds no policy —
        // just the ProtectedCursor mechanics.
        void SetMouseVisibility(bool visible) override;

        // ----- key-state badge (KEY_SEQUENCE / KEY_TABLE) -----
        // The pane owns the accumulated state (pending chord labels,
        // key-table name stack) because the actions only carry
        // deltas. All UI thread only.
        void AppendKeySequence(winrt::hstring const& label);
        void AppendKeySequence(std::wstring label) override {
            AppendKeySequence(winrt::hstring{ label });
        }
        void ClearKeySequence() override;
        void PushKeyTable(winrt::hstring const& name);
        void PushKeyTable(std::wstring name) override {
            PushKeyTable(winrt::hstring{ name });
        }
        void PopKeyTable(bool all) override;

        // Show/hide the read-only chip (READONLY action). The write
        // blocking is core-side; this is indicator only.
        void SetReadonly(bool readonly) override;

        // Reflect SECURE_INPUT on this pane's badge. ON/OFF set the
        // state directly; TOGGLE flips it here because the pane owns
        // the indicator state (ghostty's toggle keybind carries no
        // absolute value).
        void SetSecureInput(ghostty_action_secure_input_e mode) override;

        // Show the hovered-link banner with `url`, or hide it when
        // `url` is empty. See the LinkBanner comment in the XAML for
        // why this is an in-tree overlay and not a popup (#61).
        void SetHoveredLink(winrt::hstring const& url);
        void SetHoveredLink(std::wstring url) override {
            SetHoveredLink(winrt::hstring{ url });
        }

        // Reflect the SCROLLBAR report on the overlay scrollbar
        // (#154): total scrollback rows, viewport offset, viewport
        // length. Collapses the bar when nothing is scrollable,
        // otherwise reveals it and (re)starts the idle fade.
        // Core-driven updates are guarded so the bar's ValueChanged
        // does not echo back into a scroll_to_row (the GTK apprt
        // blocks its adjustment signals the same way).
        void SetScrollbar(ghostty_action_scrollbar_s bar) override;

        // ----- search bar -----
        // ghostty drives open/close and the counts; the pane owns
        // the input box. Open focuses the box (pre-filled from
        // search_selection when `needle` is non-empty), close hides
        // it and returns focus to the terminal. Counts are -1 while
        // unknown; `selected` is 1-based.
        void StartSearch(std::wstring needle) override;
        void EndSearch() override;
        void SetSearchTotal(ptrdiff_t total) override;
        void SetSearchSelected(ptrdiff_t selected) override;

        // ----- composite-level state -----

        // If the search bar is open, put keyboard focus on its input
        // box and return true; otherwise do nothing and return false.
        // Lets the window's activation focus-restore keep the bar in
        // charge instead of dropping focus back on the terminal
        // behind it (alt-tab away and back while searching).
        bool FocusSearchIfOpen();
        // Whether the search box currently holds keyboard focus. This
        // — not "the bar is open" — is what gates terminal input: the
        // bar can stay open while the user clicks back into the
        // terminal to keep typing (#171 review), and only while the
        // box actually has focus must keystrokes and IME commits stay
        // out of the pty. SurfaceHost consults it through its input
        // gate.
        bool SearchBoxHasFocus();

        // Show/hide the opaque background underlay beneath the swap
        // chain (#69 — see the XAML comment on OpaqueUnderlay for
        // the compositing math). `bg` is the terminal's current
        // background colour so opaque mode is pixel-identical to
        // opacity 1.0.
        void SetOpaqueBackground(bool opaque, winrt::Windows::UI::Color bg);

        // Apply the resolved unfocused-split appearance from config
        // (fill = the dim-overlay colour, opacity = the alpha at
        // which to draw it). Stored locally because the overlay is
        // re-shown every time focus is lost, not just on first
        // attach. The factory calls this right after Attach so the
        // initial unfocused appearance (other tabs / siblings) is
        // already correct before any focus events fire.
        void SetUnfocusedAppearance(double overlayOpacity,
                                    winrt::Windows::UI::Color overlayFill) noexcept;

        // Toggle the unfocused-dim overlay. true = focused (overlay
        // hidden, full brightness); false = unfocused (overlay shown
        // with the cached fill / opacity). The XAML element stays
        // hit-test transparent in both states so pointer routing
        // doesn't change with focus.
        void ApplyFocusVisual(bool focused);

    private:
        // Owns the surface; see SurfaceHost.h. Created in the ctor
        // against the inner panel, never null afterwards.
        std::shared_ptr<SurfaceHost> m_host;

        // Cached unfocused-split overlay parameters resolved from
        // ghostty config. See SetUnfocusedAppearance(). The
        // SolidColorBrush is reused across hide/show transitions to
        // avoid reallocating per focus event.
        double m_unfocusedOpacity = 0.3;
        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush m_unfocusedFillBrush{ nullptr };

        // MOUSE_VISIBILITY state. m_visibleCursor caches the cursor
        // built by the last SetCursorShape call so a show can restore
        // it; m_cursorHidden gates shape updates from resurrecting a
        // hidden cursor. See SetMouseVisibility().
        bool m_cursorHidden = false;
        winrt::Microsoft::UI::Input::InputCursor m_visibleCursor{ nullptr };
        // Single writer for ProtectedCursor: composes the overlay
        // hover (Arrow), hidden state (blank), and ghostty's shape.
        void ApplyCursor();

        // SECURE_INPUT indicator state; owned here so TOGGLE can
        // flip without the dispatcher tracking anything.
        bool m_secureInput = false;

        // Key-state badge state. Rebuilds the badge text from both
        // lists on every change — the lists are tiny (a table stack
        // is 1-2 deep, a chord is 2-3 keys).
        void UpdateKeyStateBadge();
        std::vector<winrt::hstring> m_keyTables;
        std::vector<winrt::hstring> m_keySequence;

        // Scrollbar state (#154). m_scrollbarSyncing is set while
        // SetScrollbar writes the bar's properties so the resulting
        // ValueChanged is recognised as an echo and not sent back to
        // ghostty. m_scrollbarHovered keeps the bar visible while the
        // pointer is over it; the idle timer fades it otherwise.
        void SetupScrollbar();
        void RevealScrollbar();
        void FadeScrollbarIfIdle();
        bool m_scrollbarSyncing = false;
        bool m_scrollbarHovered = false;
        // Pointer is over an interactive overlay other than the
        // scrollbar (the search bar); ApplyCursor shows an Arrow.
        bool m_overlayHovered = false;
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_scrollbarFadeTimer{ nullptr };

        // Search bar state. m_searchSyncing guards the programmatic
        // pre-fill in StartSearch so TextChanged doesn't fire a
        // redundant search for text ghostty just handed us. The
        // debounce mirrors the macOS SurfaceView: needles under 3
        // chars wait 300ms (cheap keystrokes, expensive short-needle
        // scans); 3+ chars and empty go immediately. m_searchTotal /
        // m_searchSelected feed the readout.
        void SetupSearchBar();
        void SendSearchNeedle();
        void UpdateSearchCount();
        void CloseSearchFromUi();
        bool m_searchOpen = false;
        bool m_searchSyncing = false;
        ptrdiff_t m_searchTotal = -1;
        ptrdiff_t m_searchSelected = -1;
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_searchDebounce{ nullptr };
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct TerminalControl : TerminalControlT<TerminalControl, implementation::TerminalControl>
    {
    };
}
