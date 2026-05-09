#include "pch.h"
#include "TerminalControl.xaml.h"
#include "Clipboard.h"
#include "Encoding.h"
#include "KeyModifiers.h"
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
            if (!self || !self->m_editContext) return;
            self->m_editContext.NotifyFocusEnter();
        });

        LostFocus([weakSelf](auto&&, auto&&) {
            auto self = weakSelf.get();
            if (!self || !self->m_editContext) return;
            self->m_editContext.NotifyFocusLeave();
        });

        PointerMoved([weakSelf](auto&&, muxi::PointerRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface) return;
            muix::PointerPoint point = args.GetCurrentPoint(self->Panel());
            auto pos = point.Position();
            ghostty_surface_mouse_pos(self->m_surface, pos.X, pos.Y, currentMods());
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
                if (ghostty_surface_has_selection(self->m_surface)) {
                    ghostty_text_s text = {};
                    if (ghostty_surface_read_selection(self->m_surface, &text) && text.text && text.text_len > 0) {
                        Clipboard::write(self->m_hostHwnd, Encoding::toUtf16(text.text, static_cast<int>(text.text_len)));
                        ghostty_surface_free_text(self->m_surface, &text);
                    }
                    // Click-then-release without modifiers clears the
                    // selection in ghostty, matching the macOS gesture.
                    ghostty_surface_mouse_button(self->m_surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                    ghostty_surface_mouse_button(self->m_surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                    return;
                }
                btn = GHOSTTY_MOUSE_RIGHT;
            } else {
                return;
            }
            ghostty_surface_mouse_button(self->m_surface, GHOSTTY_MOUSE_PRESS, btn, currentMods());
        });

        PointerReleased([weakSelf](auto&&, muxi::PointerRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface) return;
            ghostty_surface_mouse_button(self->m_surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, currentMods());
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
            ghostty_surface_mouse_scroll(self->m_surface, 0, scrollY, smods);
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

            int vk = static_cast<int>(args.Key());
            UINT scanCode = args.KeyStatus().ScanCode;
            bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
            bool shift = GetKeyState(VK_SHIFT) & 0x8000;

            // IME is processing this key — let the EditContext handlers
            // own the composition lifecycle, and don't double-encode
            // into the pty.
            if (vk == VK_PROCESSKEY || self->m_ime.composing()) return;

            // Ctrl+C: copy selection if any, otherwise fall through to
            // ghostty_surface_key so the SIGINT path runs.
            if (ctrl && !shift && vk == 'C') {
                if (ghostty_surface_has_selection(self->m_surface)) {
                    ghostty_text_s text = {};
                    if (ghostty_surface_read_selection(self->m_surface, &text) && text.text && text.text_len > 0) {
                        Clipboard::write(self->m_hostHwnd, Encoding::toUtf16(text.text, static_cast<int>(text.text_len)));
                        ghostty_surface_free_text(self->m_surface, &text);
                    }
                    ghostty_surface_mouse_button(self->m_surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                    ghostty_surface_mouse_button(self->m_surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                    args.Handled(true);
                    return;
                }
            }

            // Ctrl+V: paste from clipboard.
            if (ctrl && !shift && vk == 'V') {
                auto utf8 = Encoding::toUtf8(Clipboard::read(self->m_hostHwnd));
                if (!utf8.empty()) {
                    ghostty_surface_text(self->m_surface, utf8.c_str(), utf8.size());
                }
                if (self->m_app) ghostty_app_tick(self->m_app);
                ghostty_surface_refresh(self->m_surface);
                args.Handled(true);
                return;
            }

            // Compute the unshifted codepoint (VK translated with no
            // modifiers held) so unicode-keyed bindings can match.
            // Without this, Binding.Set.getEvent() in
            // input/Binding.zig:2622 falls through every lookup path
            // for entries like `unicode = 't'` (ctrl+shift+t = new_tab,
            // ctrl+shift+w = close_tab, etc.) and returns null. The
            // physical-keyed bindings (ctrl+tab = next_tab,
            // ctrl+shift+arrow_left = previous_tab) already match via
            // keycode; this fixes the unicode ones.
            //
            // Text encoding stays on the separate ghostty_surface_text
            // path below — passing a `text` field into the key event
            // would double-input because encodeKey also writes utf8 to
            // the pty.
            BYTE plainState[256] = {};
            wchar_t unshiftedChars[4] = {};
            int unshiftedCount = ToUnicode(vk, scanCode, plainState, unshiftedChars, 4, 0);
            // Drain any dead-key state ToUnicode left in plainState so
            // the next real keystroke isn't affected.
            wchar_t drain[4] = {};
            ToUnicode(VK_SPACE, 0x39, plainState, drain, 4, 0);

            ghostty_input_key_s keyEvent = {};
            keyEvent.action = GHOSTTY_ACTION_PRESS;
            keyEvent.keycode = scanCode;
            if (args.KeyStatus().IsExtendedKey) keyEvent.keycode |= 0xE000;
            keyEvent.mods = currentMods();
            if (unshiftedCount > 0 && unshiftedChars[0] >= 0x20) {
                keyEvent.unshifted_codepoint = static_cast<uint32_t>(unshiftedChars[0]);
            }
            bool consumed = ghostty_surface_key(self->m_surface, keyEvent);

            // Translate to text using ToUnicode (replaces
            // CharacterReceived). Skip when the binding consumed the
            // key — otherwise ctrl+shift+t would type "T" into the pty
            // in addition to opening a new tab.
            if (!consumed) {
                BYTE kbState[256] = {};
                GetKeyboardState(kbState);
                wchar_t chars[4] = {};
                int charCount = ToUnicode(vk, scanCode, kbState, chars, 4, 0);
                if (charCount > 0 && chars[0] >= 0x20) {
                    char utf8[16] = {};
                    int len = WideCharToMultiByte(CP_UTF8, 0, chars, charCount, utf8, sizeof(utf8), nullptr, nullptr);
                    if (len > 0) {
                        ghostty_surface_text(self->m_surface, utf8, len);
                    }
                }
            }

            if (self->m_app) ghostty_app_tick(self->m_app);
            ghostty_surface_refresh(self->m_surface);
            args.Handled(true);
        });

        KeyUp([weakSelf](auto&&, muxi::KeyRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface) return;
            ghostty_input_key_s keyEvent = {};
            keyEvent.action = GHOSTTY_ACTION_RELEASE;
            keyEvent.keycode = args.KeyStatus().ScanCode;
            if (args.KeyStatus().IsExtendedKey) keyEvent.keycode |= 0xE000;
            keyEvent.mods = currentMods();
            ghostty_surface_key(self->m_surface, keyEvent);
        });
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
                                 std::shared_ptr<SwapChainAttachRequest> attachRequest)
    {
        m_app = app;
        m_surface = surface;
        m_compositionHandle = compositionHandle;
        m_hostHwnd = hostHwnd;
        m_attachRequest = std::move(attachRequest);
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
        auto weakSelf = get_weak();
        m_sizeChangedToken = Panel().SizeChanged(
            [weakSelf](auto&&, Microsoft::UI::Xaml::SizeChangedEventArgs const& args) {
                auto self = weakSelf.get();
                if (!self || !self->m_surface) return;
                auto sz = args.NewSize();
                uint32_t w = static_cast<uint32_t>(sz.Width);
                uint32_t h = static_cast<uint32_t>(sz.Height);
                if (w > 0 && h > 0) {
                    ghostty_surface_set_size(self->m_surface, w, h);
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

        if (m_editContext) {
            // Best-effort: tell the OS the EditContext is leaving focus
            // before we drop our reference. Skipping this leaves the
            // text-services manager holding a stale focus pointer until
            // GC catches up.
            m_editContext.NotifyFocusLeave();
            m_editContext = nullptr;
        }

        if (auto panel = Panel()) {
            if (m_sizeChangedToken.value != 0) {
                panel.SizeChanged(m_sizeChangedToken);
                m_sizeChangedToken = {};
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
        if (m_surface) {
            ghostty_surface_free(m_surface);
            m_surface = nullptr;
        }
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
                    ghostty_surface_preedit(self->m_surface, nullptr, 0);
                } else {
                    auto utf8 = Encoding::toUtf8(self->m_ime.text());
                    if (!utf8.empty())
                        ghostty_surface_preedit(self->m_surface, utf8.c_str(), utf8.size());
                }
            }
            if (self->m_app) ghostty_app_tick(self->m_app);
            ghostty_surface_refresh(self->m_surface);
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
                ghostty_surface_preedit(self->m_surface, nullptr, 0);
                auto utf8 = Encoding::toUtf8(self->m_ime.text());
                if (!utf8.empty()) {
                    ghostty_surface_text(self->m_surface, utf8.c_str(), utf8.size());
                }
                if (self->m_app) ghostty_app_tick(self->m_app);
                ghostty_surface_refresh(self->m_surface);
            }
            self->m_ime.compositionCompleted();
        });

        m_editContext.LayoutRequested([weakSelf](
            txtCore::CoreTextEditContext const&,
            txtCore::CoreTextLayoutRequestedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface || !self->m_hostHwnd) return;
            double x = 0, y = 0, w = 0, h = 0;
            ghostty_surface_ime_point(self->m_surface, &x, &y, &w, &h);
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
                    ghostty_surface_preedit(self->m_surface, nullptr, 0);
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
}
