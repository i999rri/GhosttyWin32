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

        // Self-focus on Loaded. SelectedItem-driven focus from the
        // outside (MainWindow's SelectionChanged handler) fires while
        // TabView's content presenter is still swapping us in, and
        // Focus() returns false before layout completes. Loaded fires
        // only once the control is actually in the live visual tree
        // and measured — at that point Focus succeeds without retry.
        Loaded([weakSelf](auto&&, auto&&) {
            auto self = weakSelf.get();
            if (!self) return;
            self->Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
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

        PointerReleased([weakSelf](auto&&, muxi::PointerRoutedEventArgs const&) {
            auto self = weakSelf.get();
            if (!self || !self->m_surface) return;
            ghostty_surface_mouse_button(self->m_surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, currentMods());
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
    }

    TerminalControl::~TerminalControl()
    {
        // Belt-and-suspenders: Tab's destructor normally calls Detach
        // first, but if construction failed mid-way we still want the
        // surface/handle to be freed. Detach is idempotent.
        Detach();
    }

    void TerminalControl::Attach(ghostty_surface_t surface,
                                 HANDLE compositionHandle,
                                 HWND hostHwnd,
                                 std::shared_ptr<SwapChainAttachRequest> attachRequest)
    {
        m_surface = surface;
        m_compositionHandle = compositionHandle;
        m_hostHwnd = hostHwnd;
        m_attachRequest = std::move(attachRequest);

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

        if (auto panel = Panel()) {
            if (m_sizeChangedToken.value != 0) {
                panel.SizeChanged(m_sizeChangedToken);
                m_sizeChangedToken = {};
            }
            if (auto native2 = panel.try_as<ISwapChainPanelNative2>()) {
                native2->SetSwapChainHandle(nullptr);
            }
        }
        if (m_surface) {
            ghostty_surface_free(m_surface);
            m_surface = nullptr;
        }
        if (m_compositionHandle) {
            CloseHandle(m_compositionHandle);
            m_compositionHandle = nullptr;
        }
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
