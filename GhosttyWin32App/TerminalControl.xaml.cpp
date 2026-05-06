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
        namespace muxi = winrt::Microsoft::UI::Xaml::Input;
        namespace muix = winrt::Microsoft::UI::Input;

        PointerMoved([this](auto&&, muxi::PointerRoutedEventArgs const& args) {
            if (!m_surface) return;
            muix::PointerPoint point = args.GetCurrentPoint(Panel());
            auto pos = point.Position();
            ghostty_surface_mouse_pos(m_surface, pos.X, pos.Y, currentMods());
        });

        PointerPressed([this](auto&&, muxi::PointerRoutedEventArgs const& args) {
            if (!m_surface) return;
            muix::PointerPoint point = args.GetCurrentPoint(Panel());
            muix::PointerPointProperties props = point.Properties();
            ghostty_input_mouse_button_e btn;
            if (props.IsLeftButtonPressed()) {
                btn = GHOSTTY_MOUSE_LEFT;
            } else if (props.IsRightButtonPressed()) {
                // Right-click: copy selection if there is one,
                // otherwise treat as a normal right button press.
                if (ghostty_surface_has_selection(m_surface)) {
                    ghostty_text_s text = {};
                    if (ghostty_surface_read_selection(m_surface, &text) && text.text && text.text_len > 0) {
                        Clipboard::write(m_hostHwnd, Encoding::toUtf16(text.text, static_cast<int>(text.text_len)));
                        ghostty_surface_free_text(m_surface, &text);
                    }
                    // Click-then-release without modifiers clears the
                    // selection in ghostty, matching the macOS gesture.
                    ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                    ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                    return;
                }
                btn = GHOSTTY_MOUSE_RIGHT;
            } else {
                return;
            }
            ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_PRESS, btn, currentMods());
        });

        PointerReleased([this](auto&&, muxi::PointerRoutedEventArgs const&) {
            if (!m_surface) return;
            ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, currentMods());
        });

        PointerWheelChanged([this](auto&&, muxi::PointerRoutedEventArgs const& args) {
            if (!m_surface) return;
            muix::PointerPoint point = args.GetCurrentPoint(Panel());
            muix::PointerPointProperties props = point.Properties();
            int delta = props.MouseWheelDelta();
            double scrollY = (double)delta / 120.0;
            ghostty_input_scroll_mods_t smods = {};
            ghostty_surface_mouse_scroll(m_surface, 0, scrollY, smods);
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

        // Capture the surface by value so the lambda doesn't dereference
        // `this` after Detach() nulls m_surface — Detach unhooks the
        // event so further fires are impossible, but defense in depth.
        ghostty_surface_t s = m_surface;
        m_sizeChangedToken = Panel().SizeChanged(
            [s](auto&&, Microsoft::UI::Xaml::SizeChangedEventArgs const& args) {
                auto sz = args.NewSize();
                uint32_t w = static_cast<uint32_t>(sz.Width);
                uint32_t h = static_cast<uint32_t>(sz.Height);
                if (w > 0 && h > 0) {
                    ghostty_surface_set_size(s, w, h);
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
