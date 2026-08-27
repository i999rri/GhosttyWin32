#pragma once

#include "TerminalControl.g.h"
#include "Ghostty/Surface.h"
#include "Host/ImeBuffer.h"
#include "Interop/Encoding.h"
#include "Win32/Clipboard.h"
#include "ghostty.h"
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <winrt/Windows.UI.Text.Core.h>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace winrt::GhosttyWin32::implementation
{
    namespace host = core::host;
    namespace interop = core::interop;
    namespace win32 = core::win32;

    // Pending UI-thread "attach this swap chain handle to the panel"
    // request. Created on the UI thread inside TabFactory::Make, kept
    // alive by both the TerminalControl (so it can cancel on teardown)
    // and the renderer-thread callback (so it survives the cross-thread
    // hop). The cancelled flag is the lifetime interlock: Detach() sets
    // it before the swap chain is destroyed; the queued
    // SetSwapChainHandle bails on it before touching the (now dead)
    // handle.
    struct SwapChainAttachRequest {
        HANDLE handle{ nullptr };
        Microsoft::UI::Xaml::Controls::SwapChainPanel panel{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueue dispatcher{ nullptr };
        std::atomic<bool> cancelled{ false };
        // Called on the UI thread after SetSwapChainHandle has bound the
        // swap chain (which now has at least one presented frame) to the
        // panel. The host uses this to switch the TabView, focus the
        // panel, etc., so the panel only becomes visible once it
        // actually has content (issue #22).
        std::function<void()> onActivated;
    };

    // Renderer-thread context for libghostty's `swap_chain_changed_cb`.
    // ghostty fires that callback on every (re-)bind / resize / DPI
    // change with the IDXGISwapChain1*; we use it to install an
    // IDXGISwapChain2::SetMatrixTransform of {1/scale, 1/scale} that
    // cancels XAML SwapChainPanel's implicit upscale of attached
    // content. Without that workaround text and the background image
    // come out at ~2x size on HiDPI displays — most visibly on
    // RDP-from-a-high-DPI-client sessions.
    //
    // Lifetime: heap-allocated on Attach, raw pointer is passed to
    // libghostty as the callback's userdata, owned by TerminalControl
    // via shared_ptr. Detach sets `cancelled` so any in-flight
    // callback no-ops; the shared_ptr is released after
    // ghostty_surface_free has returned (so the renderer thread is
    // guaranteed not to fire the callback again).
    //
    // CompositionScale is stored as an atomic so the UI-thread
    // CompositionScaleChanged handler can publish updates the
    // renderer thread reads on the next callback fire without
    // locking. Defaults to 1.0 (identity matrix) so the very first
    // fire — before the panel's CompositionScale has settled — is a
    // no-op rather than installing a bogus transform.
    struct SwapChainChangedContext {
        std::atomic<bool> cancelled{ false };
        std::atomic<double> compositionScale{ 1.0 };
    };

    // UserControl wrapper around a SwapChainPanel for one terminal surface.
    //
    // SwapChainPanel inherits from Grid (not Control) and is not a
    // default tab stop, so SwapChainPanel.Focus() returns false in many
    // normal contexts. Wrapping it in a UserControl with IsTabStop=true
    // gives Tab::Focus() a target that programmatic focus moves
    // actually stick to — same pattern Windows Terminal uses around
    // its TermControl.
    //
    // This control owns the ghostty_surface_t and composition-handle
    // lifetimes for one tab: TabFactory::Make() calls Attach() once
    // surface_new succeeds, and Tab's destructor calls Detach() to tear
    // everything down in the right order.
    struct TerminalControl : TerminalControlT<TerminalControl>
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

        // Wire a freshly-created ghostty surface to this control. Hooks
        // SizeChanged on the inner panel so layout changes flow into
        // ghostty_surface_set_size, and creates a CoreTextEditContext
        // bound to this control's surface so IME composition stays
        // per-tab. The attachRequest is kept so Detach() can cancel a
        // queued SetSwapChainHandle that hasn't run yet. The host HWND
        // is stashed for Win32 APIs that need a window owner (clipboard
        // read/write, IME bounds in screen coordinates). The ghostty
        // app handle is needed to drive ghostty_app_tick after IME
        // commits so the renderer wakes promptly.
        void Attach(ghostty_app_t app,
                    ghostty_surface_t surface,
                    HANDLE compositionHandle,
                    HWND hostHwnd,
                    std::shared_ptr<SwapChainAttachRequest> attachRequest,
                    std::shared_ptr<SwapChainChangedContext> swapChainChangedContext);

        // Forwarded by MainWindow's window-Activated handler. XAML's
        // GotFocus / LostFocus don't fire on window de/activation
        // (focus is logically retained on the focused element across
        // alt-tab), so the host has to ping the active control whenever
        // the window crosses the activation boundary.
        void NotifyImeFocusEnter();
        void NotifyImeFocusLeave();

        // Tear-down counterpart of Attach. Idempotent — calling twice
        // (e.g. once from Tab::~Tab and once from ~TerminalControl) is
        // safe.
        void Detach();

        // Re-point this control at a new host window after a tab
        // tear-out / adopt. The surface and swap chain move as-is —
        // the SwapChainPanel keeps its composition binding across
        // reparenting on the shared UI thread — but two things are
        // derived from the owning window and must follow it: the host
        // HWND (IME caret coordinates via ClientToScreen, clipboard
        // ownership) and the focused callback, which feeds the owning
        // window's active-surface cache.
        void Rehost(HWND hostHwnd,
                    std::function<void(ghostty_surface_t)> onFocused) noexcept {
            m_hostHwnd  = hostHwnd;
            m_onFocused = std::move(onFocused);
        }

        // Renderer-thread callback registered with ghostty as
        // cfg.swap_chain_ready_cb. Hops to the UI thread and binds the
        // swap chain handle to the panel via ISwapChainPanelNative2.
        // Called as a free function with the SwapChainAttachRequest
        // pointer as userdata — no `this` involved.
        static void OnSwapChainReady(void* userdata) noexcept;

        // Renderer-thread callback registered with ghostty as
        // cfg.swap_chain_changed_cb. Fired after initial bind, after
        // every dx_resize, and after every DPI / font change.
        // userdata is a raw pointer to a SwapChainChangedContext
        // owned by a shared_ptr on this control. Queries the swap
        // chain to IDXGISwapChain2 and installs a (1/scale, 1/scale)
        // SetMatrixTransform that cancels XAML SwapChainPanel's
        // implicit upscale. No `this` involved.
        static void OnSwapChainChanged(void* swap_chain, void* userdata) noexcept;

        // Implementation-only accessors used by Tab.
        //
        // Surface() returns the wrapper itself so call sites can issue
        // typed operations (Refresh, Key, MouseButton, …) without
        // touching raw ghostty C API. Identity-comparison call sites
        // (action callbacks, FindPaneBySurface) compare via
        // `tc->Surface().Handle()` against ghostty's raw handle.
        core::ghostty::Surface const& Surface() const noexcept { return m_surface; }
        core::ghostty::Surface& Surface() noexcept { return m_surface; }
        HANDLE CompositionHandle() const noexcept { return m_compositionHandle; }

        // Apply a ghostty-requested mouse cursor shape. Must be called on
        // the UI thread (the caller in MainWindow::action_cb dispatches
        // via DispatcherQueue). Unrecognised shapes fall back to Arrow.
        // Used both for the initial IBeam set in the ctor and for live
        // updates as the pointer moves over/off cells, links, resize
        // borders, etc.
        void SetCursorShape(ghostty_action_mouse_shape_e shape);

        // ----- key-state badge (KEY_SEQUENCE / KEY_TABLE) -----
        // The pane owns the accumulated state (pending chord labels,
        // key-table name stack) because the actions only carry
        // deltas. All UI thread only.
        void AppendKeySequence(winrt::hstring const& label);
        void ClearKeySequence();
        void PushKeyTable(winrt::hstring const& name);
        void PopKeyTable(bool all);

        // Show/hide the opaque background underlay beneath the swap
        // chain (#69 — see the XAML comment on OpaqueUnderlay for
        // the compositing math). `bg` is the terminal's current
        // background colour so opaque mode is pixel-identical to
        // opacity 1.0. UI thread only.
        void SetOpaqueBackground(bool opaque, winrt::Windows::UI::Color bg);

        // Show/hide the read-only chip (READONLY action). The write
        // blocking is core-side; this is indicator only. UI thread.
        void SetReadonly(bool readonly);

        // Reflect SECURE_INPUT on this pane's badge. ON/OFF set the
        // state directly; TOGGLE flips it here because the pane owns
        // the indicator state (ghostty's toggle keybind carries no
        // absolute value). UI thread only.
        void SetSecureInput(ghostty_action_secure_input_e mode);

        // Mirror ghostty's MOUSE_VISIBILITY on this pane: false hides
        // the pointer (mouse-hide-while-typing), true restores the
        // last MOUSE_SHAPE. UI thread only. ghostty drives both
        // directions (hide on keypress, show on pointer move), so
        // this holds no policy — just the ProtectedCursor mechanics.
        void SetMouseVisibility(bool visible);

        // Show the hovered-link banner with `url`, or hide it when
        // `url` is empty. UI thread only — the caller dispatches from
        // the renderer thread. See the LinkBanner comment in the XAML
        // for why this is an in-tree overlay and not a popup (#61).
        void SetHoveredLink(winrt::hstring const& url);

        // Reflect the SCROLLBAR report on the overlay scrollbar
        // (#154): total scrollback rows, viewport offset, viewport
        // length. Collapses the bar when nothing is scrollable,
        // otherwise reveals it and (re)starts the idle fade. UI
        // thread only. Core-driven updates are guarded so the bar's
        // ValueChanged does not echo back into a scroll_to_row
        // (the GTK apprt blocks its adjustment signals the same way).
        void SetScrollbar(ghostty_action_scrollbar_s bar);

        // ----- search bar -----
        // ghostty drives open/close and the counts; the pane owns
        // the input box. Open focuses the box (pre-filled from
        // search_selection when `needle` is non-empty), close hides
        // it and returns focus to the terminal. Counts are -1 while
        // unknown; `selected` is 1-based. UI thread only.
        void StartSearch(std::wstring const& needle);
        void EndSearch();
        void SetSearchTotal(ptrdiff_t total);
        void SetSearchSelected(ptrdiff_t selected);
        // If the bar is open, put keyboard focus on its input box and
        // return true; otherwise do nothing and return false. Lets
        // the window's activation focus-restore keep the bar in
        // charge instead of dropping focus back on the terminal
        // behind it (alt-tab away and back while searching).
        bool FocusSearchIfOpen();
        // Whether the search box currently holds keyboard focus. This
        // — not "the bar is open" — is what gates terminal input: the
        // bar can stay open while the user clicks back into the
        // terminal to keep typing (#171 review), and only while the
        // box actually has focus must keystrokes and IME commits stay
        // out of the pty.
        bool SearchBoxHasFocus();

        // Set the callback that fires when this control receives
        // keyboard focus. Passed the underlying ghostty surface so
        // the host can update its "currently focused surface"
        // tracking without reaching into MainWindow globals from
        // here. The callback is invoked on the UI thread (XAML
        // GotFocus delivery path). Setting an empty function clears
        // the registration.
        void SetOnFocused(std::function<void(ghostty_surface_t)> cb) noexcept {
            m_onFocused = std::move(cb);
        }

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
        // Builds the per-control CoreTextEditContext and wires its
        // seven event handlers (TextRequested / SelectionRequested /
        // TextUpdating / CompositionStarted / CompositionCompleted /
        // LayoutRequested / FocusRemoved). Called from Attach once the
        // surface and HWND are both valid.
        void SetupImeContext();

        ghostty_app_t m_app{ nullptr };
        core::ghostty::Surface m_surface{};
        HANDLE m_compositionHandle{ nullptr };
        // Host window HWND — used for Win32 APIs that need a window
        // owner (clipboard read/write, IME bounds in screen coords).
        // Same value across every TerminalControl in this window;
        // stored locally to avoid reaching into MainWindow globals
        // from input handlers.
        HWND m_hostHwnd{ nullptr };
        winrt::event_token m_sizeChangedToken{};
        // SwapChainPanel composition scale (DPI / per-monitor scale)
        // tracking. The panel's CompositionScale can lag behind the
        // window's DPI on RDP — initially the panel reports 1.0 even
        // when the desktop is at 200% — and only settles once the
        // composition pipeline has picked it up. Subscribing here per-
        // leaf is more reliable than driving everything from
        // MainWindow's XamlRoot.Changed broadcast: it fires exactly
        // when the panel's own display scale changes, and the value we
        // read back is what the panel will actually composite at.
        winrt::event_token m_compositionScaleChangedToken{};
        std::shared_ptr<SwapChainAttachRequest> m_attachRequest;
        // Renderer-thread context for libghostty's swap_chain_changed_cb.
        // See SwapChainChangedContext docs at the top of this file.
        std::shared_ptr<SwapChainChangedContext> m_swapChainChangedContext;

        // IME plumbing. Each TerminalControl gets its own EditContext
        // so a composition started in one tab doesn't leak preedit
        // updates to another tab's surface when the user switches.
        // CoreTextServicesManager allows multiple EditContexts in a
        // single view; only one receives input at a time, controlled
        // via NotifyFocusEnter/Leave on tab switches and window
        // activation.
        host::ImeBuffer m_ime;
        winrt::Windows::UI::Text::Core::CoreTextEditContext m_editContext{ nullptr };

        // Notify-on-focus callback registered by the factory that built
        // this control. See SetOnFocused().
        std::function<void(ghostty_surface_t)> m_onFocused;

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
        // Single writer for ProtectedCursor: composes the scrollbar
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
