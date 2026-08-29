#include "pch.h"
#include "Terminal/SurfaceHost.h"
#include "Interop/Encoding.h"
#include "Host/KeyModifiers.h"
#include "Input/KeyEventTranslator.h"
#include "Input/TerminalKeyDown.h"
#include "Input/TerminalKeyUp.h"
#include "Display/PhysicalPixels.h"
#include "Win32/Clipboard.h"
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <dxgi1_3.h>

namespace winrt::GhosttyWin32::implementation
{
    namespace muxi = winrt::Microsoft::UI::Xaml::Input;
    namespace muix = winrt::Microsoft::UI::Input;

    // ----- lifetime -----

    void SurfaceHost::Attach(ghostty_app_t app,
                             ghostty_surface_t surface,
                             HANDLE compositionHandle,
                             HWND hostHwnd,
                             std::shared_ptr<SwapChainAttachRequest> attachRequest,
                             std::shared_ptr<SwapChainChangedContext> swapChainChangedContext)
    {
        m_app = app;
        m_surface = core::ghostty::Surface(surface);
        m_compositionHandle = compositionHandle;
        m_hostHwnd = hostHwnd;
        m_attachRequest = std::move(attachRequest);
        m_swapChainChangedContext = std::move(swapChainChangedContext);
        WireIme();

        // Capture a weak_ptr instead of `this` or the raw surface
        // pointer. Detach unhooks SizeChanged before
        // ghostty_surface_free, so in steady state the handler never
        // fires on a dead surface — but XAML can deliver a queued
        // SizeChanged after Detach during teardown, so we recheck
        // m_surface inside the handler under a strong lock.
        std::weak_ptr<SurfaceHost> weak = weak_from_this();
        m_sizeChangedToken = m_panel.SizeChanged(
            [weak](Windows::Foundation::IInspectable const& sender,
                   Microsoft::UI::Xaml::SizeChangedEventArgs const& args) {
                auto self = weak.lock();
                if (!self || !self->m_surface) return;
                // SizeChangedEventArgs.NewSize is in DIPs; ghostty's
                // surface_set_size needs the swap-chain buffer
                // resolution in physical pixels. display::ToPhysicalPixels
                // does the multiplication and the CompositionScale=0
                // fallback.
                auto sz = args.NewSize();
                auto panel = sender.as<Microsoft::UI::Xaml::Controls::SwapChainPanel>();
                auto px = display::ToPhysicalPixels(panel, sz.Width, sz.Height);
                if (px.width > 0 && px.height > 0) {
                    self->m_surface.SetSize(px.width, px.height);
                }
            });

        // Follow the panel's actual composition scale. On RDP and on
        // first-launch this lags behind the window DPI — the panel
        // initially reports 1.0 even when GetDpiForWindow says 192
        // — and only settles to the real value after the composition
        // pipeline has picked it up. Without this hook, the swap chain
        // created by ghostty at the higher scale_factor would be
        // composited at the panel's lower scale, causing the rendered
        // content (in particular text) to appear at roughly double
        // size. Same weak_ptr + surface recheck guard as SizeChanged.
        m_compositionScaleChangedToken = m_panel.CompositionScaleChanged(
            [weak](Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel, auto&&) {
                auto self = weak.lock();
                if (!self || !self->m_surface) return;
                double sx = panel.CompositionScaleX();
                double sy = panel.CompositionScaleY();
                if (sx <= 0.0 || sy <= 0.0) return;
                // Publish the new scale to the renderer-thread callback so
                // the next swap_chain_changed_cb fire installs an
                // up-to-date inverse-scale matrix. Atomic store; the
                // callback reads it next time libghostty triggers it
                // (e.g. via the set_content_scale below flowing into
                // setFontGrid -> applyFontDpiToTransforms).
                if (self->m_swapChainChangedContext) {
                    self->m_swapChainChangedContext->compositionScale.store(
                        sx, std::memory_order_release);
                }
                self->m_surface.SetContentScale(sx, sy);
                // When the composition scale changes, the panel's DIP
                // size hasn't necessarily changed (so XAML may not fire
                // SizeChanged) but the physical-pixel footprint has.
                // Re-push the size so the swap-chain resolution matches
                // the new scale; otherwise glyphs end up at the new
                // font size on the old (lower-resolution) swap chain
                // and read as oversized.
                auto px = display::MeasuredPhysical(panel);
                if (px.width > 0 && px.height > 0) {
                    self->m_surface.SetSize(px.width, px.height);
                }
            });
    }

    void SurfaceHost::Detach()
    {
        // Cancel the pending SetSwapChainHandle dispatch before we tear
        // down the swap chain — otherwise the queued call could attach
        // a freed handle to the panel after we've destroyed everything.
        if (m_attachRequest) {
            m_attachRequest->cancelled.store(true);
            m_attachRequest.reset();
        }

        // Stop the swap-chain-changed callback from touching the swap
        // chain before libghostty drops it (ghostty_surface_free below
        // joins the renderer thread, so any fire that races us no-ops).
        // We deliberately hold the shared_ptr until *after* surface_free
        // so the renderer thread can dereference the userdata safely
        // through the in-flight call — the shared_ptr is released
        // automatically when this host is destroyed.
        if (m_swapChainChangedContext) {
            m_swapChainChangedContext->cancelled.store(true);
        }

        m_ime.Reset();
        m_editContext.Release();

        if (m_panel) {
            if (m_sizeChangedToken.value != 0) {
                m_panel.SizeChanged(m_sizeChangedToken);
                m_sizeChangedToken = {};
            }
            if (m_compositionScaleChangedToken.value != 0) {
                m_panel.CompositionScaleChanged(m_compositionScaleChangedToken);
                m_compositionScaleChangedToken = {};
            }
            // We deliberately skip the symmetric
            // ISwapChainPanelNative2::SetSwapChainHandle(nullptr) that
            // mirrors the attach in OnSwapChainReady. Calling it
            // during rapid Ctrl+Shift+W tab teardown reads a null
            // compositor visual at +0x1F8 inside microsoft.ui.xaml.dll
            // and AVs. The panel keeps a reference to the (about-to-
            // be-closed) composition handle until the control is
            // released; XAML's own panel-cleanup path runs at that
            // point with the kernel handle already invalid, which it
            // tolerates without faulting.
        }
        // Wrapper dtor would free anyway, but the renderer-thread join
        // happens inside ghostty_surface_free and must run BEFORE
        // m_swapChainChangedContext goes out of scope — so we drive the
        // free explicitly here at the right point in the Detach
        // sequence rather than waiting for the dtor.
        m_surface.Reset();
        if (m_compositionHandle) {
            CloseHandle(m_compositionHandle);
            m_compositionHandle = nullptr;
        }
        m_app = nullptr;
    }

    void SurfaceHost::Tick()
    {
        if (m_app) ghostty_app_tick(m_app);
        m_surface.Refresh();
    }

    // ----- focus / IME -----

    void SurfaceHost::OnFocusGained()
    {
        // Focus is somewhere in this pane. If the terminal is what
        // receives text, this context becomes the field the OS types
        // into; if an overlay (the search box) holds the keyboard, it
        // must not — its text would be routed through the terminal's
        // IME path into the pty (#171).
        if (TerminalOwnsInput()) m_editContext.Engage();
        else                     m_editContext.Disengage();
        // Surface-level focus event for the window. Mirrors the
        // upstream getActiveSurface pattern (#62): the window uses
        // this to retarget the tab's active pane without the host
        // reaching into window globals.
        if (m_onFocused && m_surface) {
            m_onFocused(m_surface.Handle());
        }
        // Tell the renderer thread this surface is the focused one.
        // ghostty defaults every surface to focused, so without this
        // the losing pane's renderer keeps the fast poll cadence and
        // keeps blink-presenting alongside the gaining one.
        m_surface.SetFocus(true);
    }

    void SurfaceHost::OnFocusLost()
    {
        // Focus left this pane entirely.
        m_editContext.Disengage();
        m_surface.SetFocus(false);
    }

    void SurfaceHost::NotifyImeFocusEnter()
    {
        // The window came to the front with this pane active. Same
        // rule as OnFocusGained: only if the terminal, not an
        // overlay, is what receives text.
        if (TerminalOwnsInput()) m_editContext.Engage();
        else                     m_editContext.Disengage();
    }

    void SurfaceHost::NotifyImeFocusLeave()
    {
        // The window went behind another.
        m_editContext.Disengage();
    }

    void SurfaceHost::WireIme()
    {
        // The session speaks the text-services protocol; these three
        // callbacks are what the text means for the surface. Wired
        // here (not in the ctor) so they can hold a weak_ptr to this
        // host.
        std::weak_ptr<SurfaceHost> weak = weak_from_this();
        m_ime.SetOnPreedit([weak](std::string const& utf8) {
            auto self = weak.lock();
            if (!self || !self->m_surface) return;
            if (utf8.empty()) self->m_surface.Preedit(nullptr, 0);
            else              self->m_surface.Preedit(utf8.c_str(), utf8.size());
            self->Tick();
        });
        m_ime.SetOnCommit([weak](std::string const& utf8) {
            auto self = weak.lock();
            if (!self || !self->m_surface) return;
            self->m_surface.Preedit(nullptr, 0);
            // A composition committed while a sibling overlay owns
            // text input must not land in the pty (see
            // OnFocusGained).
            if (!utf8.empty() && self->TerminalOwnsInput()) {
                self->m_surface.Text(utf8.c_str(), utf8.size());
            }
            self->Tick();
        });
        m_ime.SetCaretRect([weak]() -> std::optional<winrt::Windows::Foundation::Rect> {
            auto self = weak.lock();
            if (!self || !self->m_surface || !self->m_hostHwnd) return std::nullopt;
            double x = 0, y = 0, w = 0, h = 0;
            self->m_surface.ImePoint(&x, &y, &w, &h);
            POINT screenPt = { (LONG)x, (LONG)y };
            ClientToScreen(self->m_hostHwnd, &screenPt);
            return winrt::Windows::Foundation::Rect{
                (float)screenPt.x, (float)screenPt.y, (float)w, (float)h };
        });
    }

    // ----- input translation -----

    void SurfaceHost::CopySelectionToClipboard()
    {
        ghostty_text_s text = {};
        if (m_surface.ReadSelection(&text) && text.text && text.text_len > 0) {
            win32::Clipboard::write(m_hostHwnd,
                interop::Encoding::toUtf16(text.text, static_cast<int>(text.text_len)));
            m_surface.FreeText(&text);
        }
        // Click-then-release without modifiers clears the selection
        // in ghostty, matching the macOS gesture.
        m_surface.MouseButton(GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
        m_surface.MouseButton(GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
    }

    void SurfaceHost::OnPointerMoved(muxi::PointerRoutedEventArgs const& args)
    {
        if (!m_surface) return;
        muix::PointerPoint point = args.GetCurrentPoint(m_panel);
        auto pos = point.Position();
        m_surface.MousePos(pos.X, pos.Y, host::currentMods());
    }

    void SurfaceHost::OnPointerPressed(muxi::PointerRoutedEventArgs const& args)
    {
        if (!m_surface) return;
        // Mark Handled so the event doesn't bubble into ancestor
        // focus-management code (TabViewItem / TabView / root content
        // presenter). Without this, after the composite's explicit
        // Focus(Pointer) call XAML's default routed-event handling on
        // the bubble path moves logical focus off the control,
        // LostFocus fires, and KeyDown stops being delivered until
        // focus is restored some other way.
        args.Handled(true);
        muix::PointerPoint point = args.GetCurrentPoint(m_panel);
        muix::PointerPointProperties props = point.Properties();
        ghostty_input_mouse_button_e btn;
        if (props.IsLeftButtonPressed()) {
            btn = GHOSTTY_MOUSE_LEFT;
        } else if (props.IsRightButtonPressed()) {
            // Right-click: copy selection if there is one, otherwise
            // treat as a normal right button press.
            if (m_surface.HasSelection()) {
                CopySelectionToClipboard();
                return;
            }
            btn = GHOSTTY_MOUSE_RIGHT;
        } else {
            return;
        }
        m_surface.MouseButton(GHOSTTY_MOUSE_PRESS, btn, host::currentMods());
    }

    void SurfaceHost::OnPointerReleased(muxi::PointerRoutedEventArgs const& args)
    {
        if (!m_surface) return;
        m_surface.MouseButton(GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, host::currentMods());
        args.Handled(true);
    }

    void SurfaceHost::OnPointerWheelChanged(muxi::PointerRoutedEventArgs const& args)
    {
        if (!m_surface) return;
        muix::PointerPoint point = args.GetCurrentPoint(m_panel);
        ScrollByWheel(point.Properties().MouseWheelDelta());
        args.Handled(true);
    }

    void SurfaceHost::ScrollByWheel(int wheelDelta)
    {
        if (!m_surface) return;
        ghostty_input_scroll_mods_t smods = {};
        m_surface.MouseScroll(0, static_cast<double>(wheelDelta) / 120.0, smods);
    }

    void SurfaceHost::OnKeyDown(muxi::KeyRoutedEventArgs const& args)
    {
        if (!m_surface) return;
        // While a sibling overlay owns the keyboard (the search box),
        // never forward to the pty — its keystrokes bubble up through
        // the composite to here as well (#171 review). Left unhandled
        // on purpose so the overlay's own handling still sees the key.
        if (!TerminalOwnsInput()) return;

        input::TerminalKeyDown key(args, m_ime.Composing());

        // IME owns the composition lifecycle; don't double-encode
        // into the pty.
        if (key.isImeKeystroke()) return;

        // Copy shortcut with a live selection: write to the OS
        // clipboard and clear the selection. Ctrl+C with no
        // selection falls through to ghostty so the SIGINT path
        // runs.
        if (key.isCopyShortcut() && m_surface.HasSelection()) {
            CopySelectionToClipboard();
            args.Handled(true);
            return;
        }

        // Paste shortcut: read the OS clipboard and feed it as
        // text. The paste API (ghostty_surface_text) is the
        // bracketed-paste path on ghostty's side.
        if (key.isPasteShortcut()) {
            auto utf8 = interop::Encoding::toUtf8(win32::Clipboard::read(m_hostHwnd));
            if (!utf8.empty()) {
                m_surface.Text(utf8.c_str(), utf8.size());
            }
            Tick();
            args.Handled(true);
            return;
        }

        // Forward as ordinary terminal input. textBuf owns the
        // OS-translated UTF-8 the RawKeyPress.text pointer
        // references, so it has to outlive `raw`.
        char textBuf[16] = {};
        auto raw = key.toRawKeyPress(textBuf, sizeof(textBuf));
        auto keyEvent = core::input::Translate(raw);
        m_surface.Key(keyEvent);

        Tick();
        args.Handled(true);
    }

    void SurfaceHost::OnKeyUp(muxi::KeyRoutedEventArgs const& args)
    {
        if (!m_surface) return;
        if (!TerminalOwnsInput()) return;  // see OnKeyDown
        input::TerminalKeyUp key(args);
        auto raw = key.toRawKeyRelease();
        auto keyEvent = core::input::Translate(raw);
        m_surface.Key(keyEvent);
    }

    // ----- renderer-thread callbacks -----

    void SurfaceHost::OnSwapChainReady(void* userdata) noexcept
    {
        auto* raw = reinterpret_cast<std::shared_ptr<SwapChainAttachRequest>*>(userdata);
        std::shared_ptr<SwapChainAttachRequest> req = *raw;
        delete raw;
        if (!req || !req->dispatcher) return;
        try {
            req->dispatcher.TryEnqueue([req]() {
                if (req->cancelled.load()) return;
                // Bind the swap chain (which now has at least one
                // presented frame) to the panel, then run the host's
                // activation work. Order: handle attach → onActivated.
                // The window's onActivated typically calls SelectedItem
                // to make the panel visible — by then the panel already
                // has displayable content, closing the flicker window
                // of issue #22.
                if (auto native2 = req->panel.try_as<ISwapChainPanelNative2>()) {
                    native2->SetSwapChainHandle(req->handle);
                }
                if (req->onActivated) req->onActivated();
            });
        } catch (...) {
            // Window torn down — request is implicitly cancelled.
        }
    }

    void SurfaceHost::OnSwapChainChanged(void* swap_chain, void* userdata) noexcept
    {
        // Renderer thread. Fired by libghostty on every (re-)bind /
        // ResizeBuffers / DPI change. We undo XAML SwapChainPanel's
        // implicit upscale of the attached swap chain by installing
        // an inverse-scale matrix on the chain — the host owns this
        // policy, libghostty itself is unaware of WinUI 3.
        if (!swap_chain || !userdata) return;
        auto* ctx = static_cast<SwapChainChangedContext*>(userdata);
        if (ctx->cancelled.load(std::memory_order_acquire)) return;
        double scale = ctx->compositionScale.load(std::memory_order_acquire);
        if (scale <= 0.0) return;

        auto* sc1 = static_cast<IDXGISwapChain1*>(swap_chain);
        winrt::com_ptr<IDXGISwapChain2> sc2;
        if (FAILED(sc1->QueryInterface(IID_PPV_ARGS(sc2.put())))) return;

        DXGI_MATRIX_3X2_F matrix{};
        matrix._11 = static_cast<float>(1.0 / scale);
        matrix._22 = static_cast<float>(1.0 / scale);
        (void)sc2->SetMatrixTransform(&matrix);
    }
}
