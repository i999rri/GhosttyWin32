#include "pch.h"
#include "TerminalControl.xaml.h"
#include "Interop/Encoding.h"
#include "Host/KeyModifiers.h"
#include "Input/KeyEventTranslator.h"
#include "Input/TerminalKeyDown.h"
#include "Input/TerminalKeyUp.h"
#include "Display/PhysicalPixels.h"
#include "Win32/Clipboard.h"
#include <chrono>
#include <dxgi1_3.h>
#if __has_include("TerminalControl.g.cpp")
#include "TerminalControl.g.cpp"
#endif


namespace winrt::GhosttyWin32::implementation
{
    TerminalControl::TerminalControl()
    {
        InitializeComponent();

        // Pointer routing: the handlers early-return if no surface is
        // attached yet, so it's safe to register them in the
        // constructor before TabFactory calls Attach(). Coordinates are
        // taken relative to the inner panel (== this UserControl's
        // dimensions today, but Panel() is the explicit truth) so they
        // match what ghostty's renderer expects.
        //
        // The lambdas capture a weak_ref instead of `this`. XAML can
        // route a final pointer event during window/control teardown
        // after the impl has started destructing — a raw `this` capture
        // would dereference a dangling pointer (the AV symptom we hit:
        // microsoft.ui.xaml.dll reading near-null at the m_surface
        // offset). The weak_ref short-circuits cleanly when the impl is
        // gone; weakSelf.get() returns a strong impl com_ptr that
        // exposes private members directly via operator->.
        namespace muxi = winrt::Microsoft::UI::Xaml::Input;
        namespace muix = winrt::Microsoft::UI::Input;

        auto weakSelf = get_weak();

        // Set up IME + self-focus on Loaded. Three reasons this all
        // happens here rather than in Attach or the ctor:
        //
        //   * SelectedItem-driven focus from the outside (MainWindow's
        //     SelectionChanged handler) fires while TabView's content
        //     presenter is still swapping us in, and Focus() returns
        //     false before layout completes. Loaded fires only once
        //     the control is actually in the live visual tree and
        //     measured — at that point Focus succeeds without retry.
        //
        //   * CoreTextEditContext registration with the OS-side text-
        //     services manager only takes effect when the EditContext
        //     is created against an element that's in the live visual
        //     tree. Creating it earlier (in Attach, before
        //     TabView.SelectedItem realises us) silently fails to
        //     register, so NotifyFocusEnter doesn't engage IME — the
        //     symptom was "first tab can't toggle 半角/全角 until a
        //     second tab is created."
        //
        //   * Loaded fires once per control, after both of the above
        //     conditions are true, so the setup is naturally a single
        //     idempotent step.
        Loaded([weakSelf](auto&&, auto&&) {
            auto self = weakSelf.get();
            if (!self) return;
            if (!self->m_editContext) {
                self->SetupImeContext();
            }
            self->Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
        });

        // Mirror keyboard-focus state into the EditContext. Tab
        // switches inside the same window trip these (the losing tab's
        // TerminalControl LostFocus, the gaining tab's GotFocus) so
        // IME composition is naturally scoped to the focused tab. A
        // composition in flight on tab A pauses at LostFocus and the
        // OS does not deliver further updates until tab A's
        // EditContext is reactivated. Window-level activation crosses
        // the boundary without firing these events; MainWindow's
        // Activated handler routes through NotifyImeFocusEnter/Leave
        // for that case.
        GotFocus([weakSelf](auto&&, auto&&) {
            auto self = weakSelf.get();
            if (!self) return;
            if (self->m_editContext) self->m_editContext.NotifyFocusEnter();
            // The UnfocusedDim overlay is driven by Tab.SetActiveLeaf,
            // not by XAML focus events. Reason: the dim represents
            // "this leaf is the active split in its tab", which has
            // nothing to do with XAML keyboard focus. Hooking it on
            // GotFocus / LostFocus produces a dim flash whenever
            // focus migrates briefly (tab switch, new-tab creation,
            // alt-tab away), even though the active leaf hasn't
            // changed. The surface-focused notification still fires
            // here — the host's NotifySurfaceFocused routes through
            // Tab.SetActiveLeaf, so a real focus shift (pointer
            // click on a non-active pane) still updates the dim by
            // going through the tab.
            //
            // Surface-level focus event for the host. Mirrors the
            // upstream getActiveSurface pattern (#62): the host uses
            // this to track "currently focused surface" without us
            // reaching into MainWindow globals from inside the
            // control.
            if (self->m_onFocused && self->m_surface) {
                self->m_onFocused(self->m_surface.Handle());
            }
            // Tell the renderer thread this surface is the focused
            // one. ghostty defaults every surface to focused, so
            // without this the losing pane's renderer keeps the fast
            // poll cadence and keeps blink-presenting alongside the
            // gaining one.
            self->m_surface.SetFocus(true);
        });

        LostFocus([weakSelf](auto&&, auto&&) {
            auto self = weakSelf.get();
            if (!self) return;
            if (self->m_editContext) self->m_editContext.NotifyFocusLeave();
            // No dim change here — see the GotFocus comment above.
            self->m_surface.SetFocus(false);
        });

        PointerMoved([weakSelf](auto&&, muxi::PointerRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface) return;
            muix::PointerPoint point = args.GetCurrentPoint(self->Panel());
            auto pos = point.Position();
            self->m_surface.MousePos(pos.X, pos.Y, host::currentMods());
        });

        PointerPressed([weakSelf](auto&&, muxi::PointerRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface) return;
            // Mark Handled up front so the event doesn't bubble into
            // ancestor focus-management code (TabViewItem / TabView /
            // root content presenter, depending on layout). Without
            // this, after our explicit Focus(Pointer) call XAML's
            // default routed-event handling on the bubble path moves
            // logical focus off the TerminalControl, LostFocus fires,
            // and KeyDown stops being delivered until focus is restored
            // some other way (alt-tab, Tab key, new tab). Calling Focus
            // here covers initial focus claim; Handled(true) keeps it.
            self->Focus(Microsoft::UI::Xaml::FocusState::Pointer);
            args.Handled(true);
            muix::PointerPoint point = args.GetCurrentPoint(self->Panel());
            muix::PointerPointProperties props = point.Properties();
            ghostty_input_mouse_button_e btn;
            if (props.IsLeftButtonPressed()) {
                btn = GHOSTTY_MOUSE_LEFT;
            } else if (props.IsRightButtonPressed()) {
                // Right-click: copy selection if there is one,
                // otherwise treat as a normal right button press.
                if (self->m_surface.HasSelection()) {
                    ghostty_text_s text = {};
                    if (self->m_surface.ReadSelection(&text) && text.text && text.text_len > 0) {
                        win32::Clipboard::write(self->m_hostHwnd, interop::Encoding::toUtf16(text.text, static_cast<int>(text.text_len)));
                        self->m_surface.FreeText(&text);
                    }
                    // Click-then-release without modifiers clears the
                    // selection in ghostty, matching the macOS gesture.
                    self->m_surface.MouseButton(GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                    self->m_surface.MouseButton(GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                    return;
                }
                btn = GHOSTTY_MOUSE_RIGHT;
            } else {
                return;
            }
            self->m_surface.MouseButton(GHOSTTY_MOUSE_PRESS, btn, host::currentMods());
        });

        PointerReleased([weakSelf](auto&&, muxi::PointerRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface) return;
            self->m_surface.MouseButton(GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, host::currentMods());
            args.Handled(true);
        });

        PointerWheelChanged([weakSelf](auto&&, muxi::PointerRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface) return;
            muix::PointerPoint point = args.GetCurrentPoint(self->Panel());
            muix::PointerPointProperties props = point.Properties();
            int delta = props.MouseWheelDelta();
            double scrollY = (double)delta / 120.0;
            ghostty_input_scroll_mods_t smods = {};
            self->m_surface.MouseScroll(0, scrollY, smods);
            args.Handled(true);
        });

        // KeyDown / KeyUp on the control itself: the events fire only
        // while focus is inside us, so the handlers feed directly into
        // m_surface without an ActiveControl() lookup. Args.Handled(true)
        // suppresses bubbling, which in particular prevents the
        // TabView's built-in keybindings from also acting on the
        // already-routed key.
        KeyDown([weakSelf](auto&&, muxi::KeyRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface) return;

            input::TerminalKeyDown key(args, self->m_ime.composing());

            // IME owns the composition lifecycle; don't double-encode
            // into the pty.
            if (key.isImeKeystroke()) return;

            // Copy shortcut with a live selection: write to the OS
            // clipboard and clear the selection. Ctrl+C with no
            // selection falls through to ghostty so the SIGINT path
            // runs.
            if (key.isCopyShortcut()
                && self->m_surface.HasSelection())
            {
                ghostty_text_s text = {};
                if (self->m_surface.ReadSelection(&text) && text.text && text.text_len > 0) {
                    win32::Clipboard::write(self->m_hostHwnd, interop::Encoding::toUtf16(text.text, static_cast<int>(text.text_len)));
                    self->m_surface.FreeText(&text);
                }
                self->m_surface.MouseButton(GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                self->m_surface.MouseButton(GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                args.Handled(true);
                return;
            }

            // Paste shortcut: read the OS clipboard and feed it as
            // text. The paste API (ghostty_surface_text) is the
            // bracketed-paste path on ghostty's side.
            if (key.isPasteShortcut()) {
                auto utf8 = interop::Encoding::toUtf8(win32::Clipboard::read(self->m_hostHwnd));
                if (!utf8.empty()) {
                    self->m_surface.Text(utf8.c_str(), utf8.size());
                }
                if (self->m_app) ghostty_app_tick(self->m_app);
                self->m_surface.Refresh();
                args.Handled(true);
                return;
            }

            // Forward as ordinary terminal input. textBuf owns the
            // OS-translated UTF-8 the RawKeyPress.text pointer
            // references, so it has to outlive `raw`.
            char textBuf[16] = {};
            auto raw = key.toRawKeyPress(textBuf, sizeof(textBuf));
            auto keyEvent = core::input::Translate(raw);
            self->m_surface.Key(keyEvent);

            if (self->m_app) ghostty_app_tick(self->m_app);
            self->m_surface.Refresh();
            args.Handled(true);
        });

        KeyUp([weakSelf](auto&&, muxi::KeyRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface) return;
            input::TerminalKeyUp key(args);
            auto raw = key.toRawKeyRelease();
            auto keyEvent = core::input::Translate(raw);
            self->m_surface.Key(keyEvent);
        });

        // Terminals default to a text-input cursor; ghostty issues a
        // MOUSE_SHAPE = TEXT request once the surface is wired up, but
        // setting it here too avoids a flash of arrow cursor between
        // window show and the first ghostty tick.
        SetCursorShape(GHOSTTY_MOUSE_SHAPE_TEXT);
    }

    void TerminalControl::SetCursorShape(ghostty_action_mouse_shape_e shape)
    {
        using muxi = winrt::Microsoft::UI::Input::InputSystemCursorShape;
        muxi mapped;
        switch (shape) {
            case GHOSTTY_MOUSE_SHAPE_TEXT:
            case GHOSTTY_MOUSE_SHAPE_VERTICAL_TEXT:    mapped = muxi::IBeam; break;
            case GHOSTTY_MOUSE_SHAPE_POINTER:          mapped = muxi::Hand; break;
            case GHOSTTY_MOUSE_SHAPE_HELP:             mapped = muxi::Help; break;
            case GHOSTTY_MOUSE_SHAPE_WAIT:             mapped = muxi::Wait; break;
            case GHOSTTY_MOUSE_SHAPE_PROGRESS:         mapped = muxi::AppStarting; break;
            case GHOSTTY_MOUSE_SHAPE_CROSSHAIR:        mapped = muxi::Cross; break;
            case GHOSTTY_MOUSE_SHAPE_NOT_ALLOWED:
            case GHOSTTY_MOUSE_SHAPE_NO_DROP:          mapped = muxi::UniversalNo; break;
            case GHOSTTY_MOUSE_SHAPE_ALL_SCROLL:       mapped = muxi::SizeAll; break;
            case GHOSTTY_MOUSE_SHAPE_N_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_S_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_NS_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_ROW_RESIZE:       mapped = muxi::SizeNorthSouth; break;
            case GHOSTTY_MOUSE_SHAPE_E_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_W_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_EW_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_COL_RESIZE:       mapped = muxi::SizeWestEast; break;
            case GHOSTTY_MOUSE_SHAPE_NE_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_SW_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_NESW_RESIZE:      mapped = muxi::SizeNortheastSouthwest; break;
            case GHOSTTY_MOUSE_SHAPE_NW_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_SE_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_NWSE_RESIZE:      mapped = muxi::SizeNorthwestSoutheast; break;
            default:                                   mapped = muxi::Arrow; break;
        }
        ProtectedCursor(winrt::Microsoft::UI::Input::InputSystemCursor::Create(mapped));
    }

    void TerminalControl::SetUnfocusedAppearance(double overlayOpacity,
                                                  winrt::Windows::UI::Color overlayFill) noexcept
    {
        m_unfocusedOpacity = overlayOpacity;
        // Build the brush eagerly so ApplyFocusVisual is allocation-
        // free on every focus toggle. Recreate (rather than mutate
        // the existing brush's Color) so config reloads pick up the
        // new value with a single assignment.
        m_unfocusedFillBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(overlayFill);
        // If we're currently unfocused, refresh the live overlay so a
        // reload doesn't wait until the next focus toggle to take
        // effect.
        if (auto dim = UnfocusedDim();
            dim && dim.Visibility() == winrt::Microsoft::UI::Xaml::Visibility::Visible)
        {
            dim.Fill(m_unfocusedFillBrush);
            dim.Opacity(m_unfocusedOpacity);
        }
    }

    void TerminalControl::ApplyFocusVisual(bool focused)
    {
        auto dim = UnfocusedDim();
        if (!dim) return;
        if (focused) {
            dim.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            return;
        }
        // Lazy fall-back: if the factory never called
        // SetUnfocusedAppearance (e.g. config-less unit test paths)
        // we still want something visible. Black @ 30 % matches the
        // upstream default of 0.7 opacity over a dark background.
        if (!m_unfocusedFillBrush) {
            winrt::Windows::UI::Color fallback{ 255, 0, 0, 0 };
            m_unfocusedFillBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(fallback);
        }
        dim.Fill(m_unfocusedFillBrush);
        dim.Opacity(m_unfocusedOpacity);
        dim.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
    }

    TerminalControl::~TerminalControl()
    {
        // Belt-and-suspenders: Tab's destructor normally calls Detach
        // first, but if construction failed mid-way we still want the
        // surface/handle to be freed. Detach is idempotent.
        Detach();
    }

    void TerminalControl::Attach(ghostty_app_t app,
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
        // IME setup is deferred to Loaded — see the Loaded handler
        // comment in the ctor. CreateEditContext only registers
        // properly when the owning element is in the live visual
        // tree, which doesn't happen until TabView.SelectedItem
        // realises us.

        // Capture a weak_ref to self instead of `this` or the raw
        // surface pointer. Detach unhooks SizeChanged before
        // ghostty_surface_free, so in steady state the handler never
        // fires on a dead surface — but XAML can deliver a queued
        // SizeChanged after Detach during teardown, so we recheck
        // m_surface inside the handler under a strong lock.
        //
        // SizeChanged fires on every WM_SIZE during a drag-resize,
        // and each SetSize triggers ghostty's renderer thread to
        // reallocate the swap chain. Firing that reallocation many
        // times per second is what produces the visible flicker
        // during a drag: DWM composites intermediate frames at each
        // in-flight buffer size while ghostty is midway through
        // resizing again. Coalesce into a single SetSize per
        // ~50 ms burst — for the fast-drag case the renderer only
        // sees the last few sizes, and the panel's own composition
        // engine (which stretches the current swap chain to the
        // panel's DIP rectangle in the meantime) produces a smooth
        // stretch instead of a flicker.
        auto weakSelf = get_weak();
        auto dq = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        m_pendingResizeTimer = dq ? dq.CreateTimer() : nullptr;
        if (m_pendingResizeTimer) {
            m_pendingResizeTimer.Interval(std::chrono::milliseconds{ 50 });
            m_pendingResizeTimer.IsRepeating(false);
            m_pendingResizeTimer.Tick([weakSelf](auto&&, auto&&) {
                auto self = weakSelf.get();
                if (!self || !self->m_surface) return;
                if (auto panel = self->Panel()) {
                    auto px = display::MeasuredPhysical(panel);
                    if (px.width > 0 && px.height > 0) {
                        self->m_surface.SetSize(px.width, px.height);
                    }
                }
            });
        }
        m_sizeChangedToken = Panel().SizeChanged(
            [weakSelf](Windows::Foundation::IInspectable const&,
                       Microsoft::UI::Xaml::SizeChangedEventArgs const&) {
                auto self = weakSelf.get();
                if (!self || !self->m_surface) return;
                // Restart the debounce window; the timer's Tick will
                // read the panel's current ActualWidth/Height and
                // push the final size. Firing SetSize inline here
                // would defeat the coalescing.
                if (self->m_pendingResizeTimer) {
                    self->m_pendingResizeTimer.Start();
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
        // size. Same weak_ref + surface recheck guard as SizeChanged.
        m_compositionScaleChangedToken = Panel().CompositionScaleChanged(
            [weakSelf](Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel, auto&&) {
                auto self = weakSelf.get();
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

    void TerminalControl::Detach()
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
        // automatically when this TerminalControl is destroyed.
        if (m_swapChainChangedContext) {
            m_swapChainChangedContext->cancelled.store(true);
        }

        if (m_editContext) {
            // Best-effort: tell the OS the EditContext is leaving focus
            // before we drop our reference. Skipping this leaves the
            // text-services manager holding a stale focus pointer until
            // GC catches up.
            m_editContext.NotifyFocusLeave();
            m_editContext = nullptr;
        }

        // Stop the SizeChanged debounce timer first — its Tick reads
        // m_surface, so letting it fire after we've dropped the
        // handle below would AV. The weak_ref inside the callback
        // guards the get() but stopping is cheap and clearer.
        if (m_pendingResizeTimer) {
            try { m_pendingResizeTimer.Stop(); }
            catch (winrt::hresult_error const&) {}
        }

        if (auto panel = Panel()) {
            if (m_sizeChangedToken.value != 0) {
                panel.SizeChanged(m_sizeChangedToken);
                m_sizeChangedToken = {};
            }
            if (m_compositionScaleChangedToken.value != 0) {
                panel.CompositionScaleChanged(m_compositionScaleChangedToken);
                m_compositionScaleChangedToken = {};
            }
            // We deliberately skip the symmetric
            // ISwapChainPanelNative2::SetSwapChainHandle(nullptr) that
            // mirrors the attach in OnSwapChainReady. Calling it
            // during rapid Ctrl+Shift+W tab teardown reads a null
            // compositor visual at +0x1F8 inside microsoft.ui.xaml.dll
            // and AVs. The panel keeps a reference to the (about-to-
            // be-closed) composition handle until the impl is
            // released; XAML's own panel-cleanup path runs at that
            // point with the kernel handle already invalid, which it
            // tolerates without faulting.
        }
        // Wrapper dtor would free anyway, but the renderer-thread join
        // happens inside ghostty_surface_free and must run BEFORE
        // m_swapChainChangedContext goes out of scope below — so we
        // drive the free explicitly here at the right point in the
        // Detach sequence rather than waiting for the dtor.
        m_surface.Reset();
        if (m_compositionHandle) {
            CloseHandle(m_compositionHandle);
            m_compositionHandle = nullptr;
        }
        m_app = nullptr;
    }

    void TerminalControl::SetupImeContext()
    {
        namespace txtCore = winrt::Windows::UI::Text::Core;
        // CoreTextServicesManager.GetForCurrentView lives at the view
        // (~window) level, but CreateEditContext spins up an
        // independent context — multiple controls in the same window
        // each get their own. The OS arbitrates which one receives
        // input via NotifyFocusEnter/Leave; we drive those on tab
        // switch (GotFocus/LostFocus) and on window activation
        // (forwarded from MainWindow via NotifyImeFocusEnter/Leave).
        auto manager = txtCore::CoreTextServicesManager::GetForCurrentView();
        m_editContext = manager.CreateEditContext();
        m_editContext.InputPaneDisplayPolicy(txtCore::CoreTextInputPaneDisplayPolicy::Manual);
        m_editContext.InputScope(txtCore::CoreTextInputScope::Default);

        auto weakSelf = get_weak();

        m_editContext.TextRequested([weakSelf](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextTextRequestedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self) return;
            args.Request().Text(winrt::hstring(self->m_ime.paddedText()));
        });

        m_editContext.SelectionRequested([weakSelf](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextSelectionRequestedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self) return;
            int32_t pos = self->m_ime.selectionPosition();
            args.Request().Selection({ pos, pos });
        });

        m_editContext.TextUpdating([weakSelf](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextTextUpdatingEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface) return;
            auto range = args.Range();
            auto newText = args.Text();
            self->m_ime.applyTextUpdate(range.StartCaretPosition, range.EndCaretPosition,
                                        newText.c_str(), newText.size());
            if (self->m_ime.composing()) {
                if (self->m_ime.text().empty()) {
                    self->m_surface.Preedit(nullptr, 0);
                } else {
                    auto utf8 = interop::Encoding::toUtf8(self->m_ime.text());
                    if (!utf8.empty())
                        self->m_surface.Preedit(utf8.c_str(), utf8.size());
                }
            }
            if (self->m_app) ghostty_app_tick(self->m_app);
            self->m_surface.Refresh();
        });

        m_editContext.CompositionStarted([weakSelf](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextCompositionStartedEventArgs const&) {
            auto self = weakSelf.get();
            if (!self) return;
            self->m_ime.compositionStarted();
        });

        m_editContext.CompositionCompleted([weakSelf](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextCompositionCompletedEventArgs const&) {
            auto self = weakSelf.get();
            if (!self) return;
            if (self->m_surface) {
                self->m_surface.Preedit(nullptr, 0);
                auto utf8 = interop::Encoding::toUtf8(self->m_ime.text());
                if (!utf8.empty()) {
                    self->m_surface.Text(utf8.c_str(), utf8.size());
                }
                if (self->m_app) ghostty_app_tick(self->m_app);
                self->m_surface.Refresh();
            }
            self->m_ime.compositionCompleted();
        });

        m_editContext.LayoutRequested([weakSelf](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextLayoutRequestedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface || !self->m_hostHwnd) return;
            double x = 0, y = 0, w = 0, h = 0;
            self->m_surface.ImePoint(&x, &y, &w, &h);
            POINT screenPt = { (LONG)x, (LONG)y };
            ClientToScreen(self->m_hostHwnd, &screenPt);
            winrt::Windows::Foundation::Rect bounds{
                (float)screenPt.x, (float)screenPt.y, (float)w, (float)h };
            args.Request().LayoutBounds().ControlBounds(bounds);
            args.Request().LayoutBounds().TextBounds(bounds);
        });

        m_editContext.FocusRemoved([weakSelf](
            txtCore::CoreTextEditContext const&, auto&&) {
            auto self = weakSelf.get();
            if (!self) return;
            if (self->m_ime.composing()) {
                self->m_ime.reset();
                if (self->m_surface)
                    self->m_surface.Preedit(nullptr, 0);
            }
        });
    }

    void TerminalControl::NotifyImeFocusEnter()
    {
        if (m_editContext) m_editContext.NotifyFocusEnter();
    }

    void TerminalControl::NotifyImeFocusLeave()
    {
        if (m_editContext) m_editContext.NotifyFocusLeave();
    }

    void TerminalControl::OnSwapChainReady(void* userdata) noexcept
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
                // Host's onActivated typically calls SelectedItem to
                // make the panel visible — by then the panel already
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

    void TerminalControl::OnSwapChainChanged(void* swap_chain, void* userdata) noexcept
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
