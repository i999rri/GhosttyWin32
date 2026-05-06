#include "pch.h"
#include "TerminalControl.xaml.h"
#if __has_include("TerminalControl.g.cpp")
#include "TerminalControl.g.cpp"
#endif

namespace winrt::GhosttyWin32::implementation
{
    TerminalControl::TerminalControl()
    {
        InitializeComponent();
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
                                 std::shared_ptr<SwapChainAttachRequest> attachRequest)
    {
        m_surface = surface;
        m_compositionHandle = compositionHandle;
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
