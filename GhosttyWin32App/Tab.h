#pragma once

#include "ghostty.h"
#include "TabId.h"
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <atomic>
#include <functional>
#include <memory>

namespace winrt::GhosttyWin32::implementation {

// Pending UI-thread "attach this swap chain handle to the panel" request.
// Created on the UI thread inside TabFactory::Make, kept alive by both
// the Tab (so it can cancel on teardown) and the renderer-thread
// callback (so it survives the cross-thread hop). The cancelled flag is
// the lifetime interlock: ~Tab sets it before the swap chain is
// destroyed; the queued SetSwapChainHandle bails on it before touching
// the (now dead) handle.
struct SwapChainAttachRequest {
    HANDLE handle{ nullptr };
    Microsoft::UI::Xaml::Controls::SwapChainPanel panel{ nullptr };
    Microsoft::UI::Dispatching::DispatcherQueue dispatcher{ nullptr };
    std::atomic<bool> cancelled{ false };
    // Called on the UI thread after SetSwapChainHandle has bound the swap
    // chain (which now has its first frame presented) to the panel. The
    // host uses this to switch the TabView, focus the panel, etc., so
    // the panel only becomes visible once it actually has content
    // (issue #22).
    std::function<void()> onActivated;
};

// One terminal tab. Existence implies fully-formed: panel attached to a
// composition surface, ghostty surface created, SizeChanged hooked.
//
// Construction is just validation + member init — all failable work
// (creating the surface handle, attaching it, calling ghostty_surface_new)
// lives in TabFactory::Make. If you have a Tab*, you can operate on it
// freely without worrying about half-built state.
class Tab {
public:
    // Called by TabFactory::Make once all resources are acquired. Public
    // so std::make_unique can see it; the constructor still validates
    // its inputs and throws on missing resources, so callers outside the
    // factory will get a loud failure rather than a silent half-built
    // Tab.
    Tab(Microsoft::UI::Xaml::Controls::SwapChainPanel panel,
        Microsoft::UI::Xaml::Controls::TabViewItem item,
        HANDLE surfaceHandle,
        ghostty_surface_t surface,
        std::shared_ptr<SwapChainAttachRequest> attachRequest,
        TabId id)
        : m_panel(std::move(panel))
        , m_item(std::move(item))
        , m_surfaceHandle(surfaceHandle)
        , m_surface(surface)
        , m_attachRequest(std::move(attachRequest))
        , m_id(id)
    {
        if (!m_panel || !m_item || !m_surfaceHandle || !m_surface) {
            throw winrt::hresult_error(E_INVALIDARG, L"Tab: missing resource");
        }
        ghostty_surface_t s = m_surface;
        m_sizeChangedToken = m_panel.SizeChanged(
            [s](auto&&, Microsoft::UI::Xaml::SizeChangedEventArgs const& args) {
                auto sz = args.NewSize();
                uint32_t w = static_cast<uint32_t>(sz.Width);
                uint32_t h = static_cast<uint32_t>(sz.Height);
                if (w > 0 && h > 0) {
                    ghostty_surface_set_size(s, w, h);
                }
            });
    }

    ~Tab() {
        // Cancel the pending SetSwapChainHandle dispatch before we tear
        // down the swap chain — otherwise the queued call could attach a
        // freed handle to the panel after we've destroyed everything.
        if (m_attachRequest) m_attachRequest->cancelled.store(true);

        if (m_panel) {
            if (m_sizeChangedToken.value != 0) {
                m_panel.SizeChanged(m_sizeChangedToken);
            }
            if (auto native2 = m_panel.try_as<ISwapChainPanelNative2>()) {
                native2->SetSwapChainHandle(nullptr);
            }
        }
        if (m_surface) ghostty_surface_free(m_surface);
        if (m_surfaceHandle) CloseHandle(m_surfaceHandle);
    }

    Tab(const Tab&) = delete;
    Tab& operator=(const Tab&) = delete;
    Tab(Tab&&) = delete;
    Tab& operator=(Tab&&) = delete;

    ghostty_surface_t Surface() const noexcept { return m_surface; }
    Microsoft::UI::Xaml::Controls::SwapChainPanel const& Panel() const noexcept { return m_panel; }
    Microsoft::UI::Xaml::Controls::TabViewItem const& Item() const noexcept { return m_item; }
    HANDLE SurfaceHandle() const noexcept { return m_surfaceHandle; }
    TabId Id() const noexcept { return m_id; }

    void Focus() {
        m_panel.Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
    }

    // Renderer-thread callback from ghostty: the swap chain is now bound to
    // our surface handle, so it's safe to call SetSwapChainHandle. We only
    // hop to the UI thread and queue the actual API call — keep this fast.
    // Public so TabFactory::Make can register it as cfg.swap_chain_ready_cb;
    // ghostty calls it directly without going through any Tab instance.
    static void OnSwapChainReady(void* userdata) noexcept {
        auto* raw = reinterpret_cast<std::shared_ptr<SwapChainAttachRequest>*>(userdata);
        std::shared_ptr<SwapChainAttachRequest> req = *raw;
        delete raw;
        if (!req || !req->dispatcher) return;
        try {
            req->dispatcher.TryEnqueue([req]() {
                if (req->cancelled.load()) return;
                // Bind the swap chain (which now has at least one presented
                // frame) to the panel, then run the host's activation work.
                // Order: handle attach → onActivated. Host's onActivated
                // typically calls SelectedItem to make the panel visible
                // — by then the panel already has displayable content,
                // closing the flicker window of issue #22.
                if (auto native2 = req->panel.try_as<ISwapChainPanelNative2>()) {
                    native2->SetSwapChainHandle(req->handle);
                }
                if (req->onActivated) req->onActivated();
            });
        } catch (...) {
            // Window torn down — request is implicitly cancelled.
        }
    }

private:
    Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
    Microsoft::UI::Xaml::Controls::TabViewItem m_item{ nullptr };
    HANDLE m_surfaceHandle{ nullptr };
    ghostty_surface_t m_surface{ nullptr };
    winrt::event_token m_sizeChangedToken{};
    std::shared_ptr<SwapChainAttachRequest> m_attachRequest;
    TabId m_id{};
};

}  // namespace winrt::GhosttyWin32::implementation
