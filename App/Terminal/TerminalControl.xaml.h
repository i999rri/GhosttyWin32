#pragma once

#include "TerminalControl.g.h"
#include "Terminal/SurfaceHost.h"
#include "Terminal/Overlays/PaneStatusOverlay.xaml.h"
#include "Terminal/Overlays/ScrollbackOverlay.xaml.h"
#include "Terminal/Overlays/SearchOverlay.xaml.h"
#include "Ghostty/Surface.h"
#include "Host/ISurfaceView.h"
#include "ghostty.h"
#include <functional>
#include <memory>

namespace winrt::GhosttyWin32::implementation
{
    // The pane's composite: one UserControl whose Grid stacks the
    // SwapChainPanel a SurfaceHost renders into with the overlays
    // layered on top. Two kinds of children, deliberately unaware of
    // each other:
    //
    //   * SurfaceHost (plain class, owned here) — the ghostty surface
    //     and everything that feeds it: lifetime, swap chain, pointer
    //     / keyboard translation, IME, clipboard.
    //   * PaneStatusOverlay / ScrollbackOverlay / SearchOverlay (XAML
    //     children) — what the pane shows around the terminal. They
    //     never touch ghostty; user intent leaves them through
    //     callbacks this composite wires to the surface.
    //
    // What stays on the composite is exactly what needs both sides:
    //   * XAML focus. Focus() on pointer press, GotFocus / LostFocus
    //     forwarded to the host, and keyboard ownership: which child
    //     has the keyboard right now. FocusedOverlay() is the single
    //     enumeration of overlays that can take focus, and the host's
    //     input gate is answered from it — the host itself cannot see
    //     its siblings.
    //   * ProtectedCursor — one writer composing ghostty's shape and
    //     the hidden state. (Overlays that take input declare their
    //     own Arrow; the composite does not track hover.)
    //   * core::host::ISurfaceView — libghostty's surface-targeted
    //     actions reach this control through the window's
    //     FindSurfaceView directory and are routed to the child that
    //     renders them.
    //   * The two rectangles that are the pane's own skin (opaque
    //     underlay, unfocused dim).
    //
    // SwapChainPanel inherits from Grid (not Control) and is not a
    // default tab stop, so SwapChainPanel.Focus() returns false in
    // many normal contexts. Wrapping it in a UserControl with
    // IsTabStop=true gives Tab::Focus() a target that programmatic
    // focus moves actually stick to — same pattern Windows Terminal
    // uses around its TermControl.
    //
    // The public Attach / Detach / Rehost / Surface() forward to the
    // host so Tab, TabFactory and MainWindow keep one handle per pane.
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
        // Cursor is composite state (see ApplyCursor); everything
        // else is routed to the overlay that renders it.

        // Apply a ghostty-requested mouse cursor shape. Unrecognised
        // shapes fall back to Arrow. Used both for the initial IBeam
        // set in the ctor and for live updates as the pointer moves
        // over/off cells, links, resize borders.
        void SetCursorShape(ghostty_action_mouse_shape_e shape) override;
        // Mirror ghostty's MOUSE_VISIBILITY: false hides the pointer
        // (mouse-hide-while-typing), true restores the last
        // MOUSE_SHAPE. ghostty drives both directions, so this holds
        // no policy — just the ProtectedCursor mechanics.
        void SetMouseVisibility(bool visible) override;

        void SetHoveredLink(std::wstring url) override;
        void SetReadonly(bool readonly) override;
        void SetSecureInput(ghostty_action_secure_input_e mode) override;
        void AppendKeySequence(std::wstring label) override;
        void ClearKeySequence() override;
        void PushKeyTable(std::wstring name) override;
        void PopKeyTable(bool all) override;
        void SetScrollbar(ghostty_action_scrollbar_s bar) override;
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
        // Implementation objects behind the x:Name'd overlay children.
        // Null only if InitializeComponent failed part-way.
        PaneStatusOverlay* StatusImpl();
        ScrollbackOverlay* ScrollbackImpl();
        SearchOverlay* SearchImpl();

        // Keyboard ownership. FocusedOverlay() returns the overlay
        // that currently holds keyboard focus, or null when the
        // terminal does — the one place focus-taking overlays are
        // enumerated. Gating on focus rather than on "the bar is
        // open" lets the user click back into the terminal and keep
        // typing with the search bar up (#171 review). A future
        // overlay that takes focus joins this function and inherits
        // every guard the host derives from it.
        Microsoft::UI::Xaml::UIElement FocusedOverlay();
        bool TerminalOwnsInput() { return FocusedOverlay() == nullptr; }

        // Owns the surface; see SurfaceHost.h. Created in the ctor
        // against the inner panel, never null afterwards.
        std::shared_ptr<SurfaceHost> m_host;

        // Cached unfocused-split overlay parameters resolved from
        // ghostty config. See SetUnfocusedAppearance(). The
        // SolidColorBrush is reused across hide/show transitions to
        // avoid reallocating per focus event.
        double m_unfocusedOpacity = 0.3;
        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush m_unfocusedFillBrush{ nullptr };

        // Cursor state. m_visibleCursor caches the cursor built by the
        // last SetCursorShape call so a show can restore it;
        // m_cursorHidden gates shape updates from resurrecting a
        // hidden cursor.
        void ApplyCursor();
        bool m_cursorHidden = false;
        winrt::Microsoft::UI::Input::InputCursor m_visibleCursor{ nullptr };
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct TerminalControl : TerminalControlT<TerminalControl, implementation::TerminalControl>
    {
    };
}
