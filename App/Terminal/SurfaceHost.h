#pragma once

#include "Ghostty/Surface.h"
#include "Terminal/EditContext.h"
#include "Terminal/ImeSession.h"
#include "Interop/Encoding.h"
#include "Win32/Clipboard.h"
#include "ghostty.h"
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <atomic>
#include <functional>
#include <memory>

namespace winrt::GhosttyWin32::implementation
{
    namespace host = core::host;
    namespace interop = core::interop;
    namespace win32 = core::win32;

    // Pending UI-thread "attach this swap chain handle to the panel"
    // request. Created on the UI thread inside TabFactory::Make, kept
    // alive by both the SurfaceHost (so it can cancel on teardown)
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
    // libghostty as the callback's userdata, owned by SurfaceHost via
    // shared_ptr. Detach sets `cancelled` so any in-flight callback
    // no-ops; the shared_ptr is released after ghostty_surface_free
    // has returned (so the renderer thread is guaranteed not to fire
    // the callback again).
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

    // Owns one ghostty surface and everything that exists only to feed
    // it: the swap chain binding to a SwapChainPanel, the pointer /
    // keyboard translation from XAML routed events into
    // ghostty_surface_* calls, the IME CoreTextEditContext, and the
    // clipboard shortcuts. It knows nothing about the pane's overlays
    // (badges, scrollbar, search bar) — those are siblings owned by
    // the TerminalControl composite, which forwards routed events here
    // after doing its own focus work.
    //
    // Not a XAML object on purpose: it borrows the panel it renders
    // into and is owned by the control through a shared_ptr, so the
    // XAML / EditContext handlers it registers capture a weak_ptr and
    // short-circuit once the host is gone (the same protection the
    // control's get_weak() gives its own handlers — XAML can route a
    // final event during teardown).
    //
    // Lifetime contract (unchanged from when this lived on the
    // control): TabFactory::Make() calls Attach() once surface_new
    // succeeds, and Detach() tears everything down in the one order
    // that is safe — cancel the queued swap-chain attach, cancel the
    // renderer-thread callback context, drop the EditContext, unhook
    // the panel events, then ghostty_surface_free (which joins the
    // renderer thread), then close the composition handle.
    class SurfaceHost : public std::enable_shared_from_this<SurfaceHost>
    {
    public:
        explicit SurfaceHost(Microsoft::UI::Xaml::Controls::SwapChainPanel panel)
            : m_panel(std::move(panel))
            , m_editContext(m_panel)
            , m_ime(m_editContext) {}
        ~SurfaceHost() { Detach(); }

        SurfaceHost(SurfaceHost const&) = delete;
        SurfaceHost& operator=(SurfaceHost const&) = delete;

        // Wire a freshly-created ghostty surface to this host. Hooks
        // SizeChanged / CompositionScaleChanged on the panel so layout
        // and DPI changes flow into ghostty_surface_set_size /
        // set_content_scale. The attachRequest is kept so Detach() can
        // cancel a queued SetSwapChainHandle that hasn't run yet. The
        // host HWND is stashed for Win32 APIs that need a window owner
        // (clipboard read/write, IME bounds in screen coordinates). The
        // ghostty app handle is needed to drive ghostty_app_tick after
        // input so the renderer wakes promptly.
        void Attach(ghostty_app_t app,
                    ghostty_surface_t surface,
                    HANDLE compositionHandle,
                    HWND hostHwnd,
                    std::shared_ptr<SwapChainAttachRequest> attachRequest,
                    std::shared_ptr<SwapChainChangedContext> swapChainChangedContext);

        // Tear-down counterpart of Attach. Idempotent.
        void Detach();

        // Re-point this host at a new window after a tab tear-out /
        // adopt. The surface and swap chain move as-is — the
        // SwapChainPanel keeps its composition binding across
        // reparenting on the shared UI thread — but two things are
        // derived from the owning window and must follow it: the host
        // HWND (IME caret coordinates via ClientToScreen, clipboard
        // ownership) and the focused callback.
        void Rehost(HWND hostHwnd,
                    std::function<void(ghostty_surface_t)> onFocused) noexcept {
            m_hostHwnd  = hostHwnd;
            m_onFocused = std::move(onFocused);
        }

        core::ghostty::Surface const& Surface() const noexcept { return m_surface; }
        core::ghostty::Surface& Surface() noexcept { return m_surface; }
        HANDLE CompositionHandle() const noexcept { return m_compositionHandle; }

        // ----- focus / IME -----

        // Callback fired (on the UI thread, from OnFocusGained) with
        // the surface handle whenever the terminal gains keyboard
        // focus. The owning window uses it to retarget the tab's
        // active pane. Empty clears the registration.
        void SetOnFocused(std::function<void(ghostty_surface_t)> cb) noexcept {
            m_onFocused = std::move(cb);
        }

        // Predicate consulted before any keystroke, IME engagement or
        // IME commit reaches the pty: true means the terminal owns
        // text input right now. The composite supplies it because the
        // answer depends on siblings this host cannot see (the search
        // box holding focus, #171). Unset means "always".
        void SetInputGate(std::function<bool()> terminalOwnsInput) noexcept {
            m_terminalOwnsInput = std::move(terminalOwnsInput);
        }

        // XAML GotFocus / LostFocus on the composite. GotFocus also
        // fires when an overlay inside the composite (the search box)
        // takes focus, because the event bubbles — so "gained" means
        // "focus is somewhere in this pane", and whether the terminal
        // itself receives text is decided by TerminalOwnsInput.
        // Gained additionally notifies the window and tells ghostty
        // this surface is focused so the losing pane's renderer drops
        // to the slow cadence.
        void OnFocusGained();
        void OnFocusLost();

        // Window-activation boundary: XAML's focus events do not fire
        // on alt-tab (logical focus stays on the element), so the
        // window tells its active pane when it comes to the front or
        // goes behind. Engagement only.
        void NotifyImeFocusEnter();
        void NotifyImeFocusLeave();

        // ----- input translation -----
        // Routed events forwarded by the composite. Coordinates are
        // taken relative to the panel so they match what ghostty's
        // renderer expects. Each is a no-op without a surface.
        void OnPointerMoved(Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPointerPressed(Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPointerReleased(Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPointerWheelChanged(Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        // Wheel from somewhere other than the panel (an overlay that
        // would otherwise consume it). Same scroll path, delta only.
        void ScrollByWheel(int wheelDelta);
        void OnKeyDown(Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
        void OnKeyUp(Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);

        // ----- renderer-thread callbacks -----

        // Registered with ghostty as cfg.swap_chain_ready_cb. Hops to
        // the UI thread and binds the swap chain handle to the panel
        // via ISwapChainPanelNative2. Called as a free function with
        // the SwapChainAttachRequest pointer as userdata.
        static void OnSwapChainReady(void* userdata) noexcept;

        // Registered with ghostty as cfg.swap_chain_changed_cb. Fired
        // after initial bind, after every dx_resize, and after every
        // DPI / font change. userdata is a raw pointer to a
        // SwapChainChangedContext owned by a shared_ptr on this host.
        // Queries the swap chain to IDXGISwapChain2 and installs a
        // (1/scale, 1/scale) SetMatrixTransform that cancels XAML
        // SwapChainPanel's implicit upscale.
        static void OnSwapChainChanged(void* swap_chain, void* userdata) noexcept;

    private:
        // Hand the IME session what its text means for this surface
        // (preedit, gated commit, caret rect). Called from Attach so
        // the callbacks can hold a weak_ptr to this host.
        void WireIme();
        bool TerminalOwnsInput() const {
            return !m_terminalOwnsInput || m_terminalOwnsInput();
        }
        void CopySelectionToClipboard();
        void Tick();

        Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
        ghostty_app_t m_app{ nullptr };
        core::ghostty::Surface m_surface{};
        HANDLE m_compositionHandle{ nullptr };
        // Host window HWND — used for Win32 APIs that need a window
        // owner (clipboard read/write, IME bounds in screen coords).
        HWND m_hostHwnd{ nullptr };
        winrt::event_token m_sizeChangedToken{};
        // SwapChainPanel composition scale (DPI / per-monitor scale)
        // tracking. The panel's CompositionScale can lag behind the
        // window's DPI on RDP — initially the panel reports 1.0 even
        // when the desktop is at 200% — and only settles once the
        // composition pipeline has picked it up. Subscribing per-leaf
        // is more reliable than driving everything from the window's
        // XamlRoot.Changed broadcast: it fires exactly when the
        // panel's own display scale changes, and the value we read
        // back is what the panel will actually composite at.
        winrt::event_token m_compositionScaleChangedToken{};
        std::shared_ptr<SwapChainAttachRequest> m_attachRequest;
        std::shared_ptr<SwapChainChangedContext> m_swapChainChangedContext;

        // IME, in two parts. EditContext wraps the CoreTextEditContext
        // — its creation timing, engagement, and the seven events —
        // and ImeSession says what happens on those events (the
        // composition buffer, preedit, commit). One pair per surface
        // so a composition started in one tab doesn't leak preedit
        // updates to another tab's surface. This host supplies what
        // the text means for the surface through the session's
        // callbacks (wired in Attach). Declaration order matters: the
        // session installs its handlers on the context in its ctor
        // and removes them in its dtor, so the context must be
        // constructed first and destroyed last.
        EditContext m_editContext;
        ImeSession m_ime;

        std::function<void(ghostty_surface_t)> m_onFocused;
        std::function<bool()> m_terminalOwnsInput;
    };
}
