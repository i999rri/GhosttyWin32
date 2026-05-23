#pragma once

#include "TerminalControl.g.h"
#include "ImeBuffer.h"
#include "ghostty.h"
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <winrt/Windows.UI.Text.Core.h>
#include <atomic>
#include <functional>
#include <memory>

namespace winrt::GhosttyWin32::implementation
{
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
                    std::shared_ptr<SwapChainAttachRequest> attachRequest);

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

        // Renderer-thread callback registered with ghostty as
        // cfg.swap_chain_ready_cb. Hops to the UI thread and binds the
        // swap chain handle to the panel via ISwapChainPanelNative2.
        // Called as a free function with the SwapChainAttachRequest
        // pointer as userdata — no `this` involved.
        static void OnSwapChainReady(void* userdata) noexcept;

        // Implementation-only accessors used by Tab.
        ghostty_surface_t Surface() const noexcept { return m_surface; }
        HANDLE CompositionHandle() const noexcept { return m_compositionHandle; }

        // Apply a ghostty-requested mouse cursor shape. Must be called on
        // the UI thread (the caller in MainWindow::action_cb dispatches
        // via DispatcherQueue). Unrecognised shapes fall back to Arrow.
        // Used both for the initial IBeam set in the ctor and for live
        // updates as the pointer moves over/off cells, links, resize
        // borders, etc.
        void SetCursorShape(ghostty_action_mouse_shape_e shape);

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
        ghostty_surface_t m_surface{ nullptr };
        HANDLE m_compositionHandle{ nullptr };
        // Host window HWND — used for Win32 APIs that need a window
        // owner (clipboard read/write, IME bounds in screen coords).
        // Same value across every TerminalControl in this window;
        // stored locally to avoid reaching into MainWindow globals
        // from input handlers.
        HWND m_hostHwnd{ nullptr };
        winrt::event_token m_sizeChangedToken{};
        std::shared_ptr<SwapChainAttachRequest> m_attachRequest;

        // IME plumbing. Each TerminalControl gets its own EditContext
        // so a composition started in one tab doesn't leak preedit
        // updates to another tab's surface when the user switches.
        // CoreTextServicesManager allows multiple EditContexts in a
        // single view; only one receives input at a time, controlled
        // via NotifyFocusEnter/Leave on tab switches and window
        // activation.
        ImeBuffer m_ime;
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
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct TerminalControl : TerminalControlT<TerminalControl, implementation::TerminalControl>
    {
    };
}
