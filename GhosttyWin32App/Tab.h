#pragma once

#include "ghostty.h"
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <dcomp.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <atomic>
#include <functional>
#include <memory>

#pragma comment(lib, "dcomp.lib")

namespace winrt::GhosttyWin32::implementation {

// Pending UI-thread "attach this swap chain handle to the panel" request.
// Created on the UI thread inside Tab::Create, kept alive by both the Tab
// (so it can cancel on teardown) and the renderer-thread callback (so it
// survives the cross-thread hop). The cancelled flag is the lifetime
// interlock: ~Tab sets it before the swap chain is destroyed; the queued
// SetSwapChainHandle bails on it before touching the (now dead) handle.
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
// Construction is just validation + member init — all failable work (creating
// the surface handle, attaching it, calling ghostty_surface_new) lives in
// the free factory `CreateTab` below. If you have a `Tab*`, you can operate
// on it freely without worrying about half-built state.
class Tab {
public:
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

    void Focus() {
        m_panel.Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
    }

    // Factory: given a panel + item, do the failable orchestration (handle
    // creation, ghostty surface creation, swap-chain attach scheduling) and
    // return a fully-formed Tab. Returns nullptr on failure (after cleaning
    // up any partially-acquired resources). Call on the UI thread; the panel
    // does NOT need to be in the visual tree yet — that's the whole point of
    // issue #22, where making the panel visible before it had displayable
    // content produced a flicker. The optional onActivated callback runs on
    // the UI thread once ghostty has presented its first frame and we've
    // bound the swap chain to the panel; the host uses it to switch the
    // TabView so the panel becomes visible only with real content.
    //
    // Ordering: the DComp surface handle is bound to the panel only AFTER
    // ghostty's renderer thread has presented at least one real frame —
    // see OnSwapChainReady. ghostty fires the swap-chain-ready callback
    // from drawFrameEnd (post first present), not from swap-chain creation,
    // so the back buffer is guaranteed to have displayable content by the
    // time we attach.
    static std::unique_ptr<Tab> Create(
        Microsoft::UI::Xaml::Controls::SwapChainPanel panel,
        Microsoft::UI::Xaml::Controls::TabViewItem item,
        ghostty_app_t app,
        HWND hwnd,
        std::function<void()> onActivated = {},
        uint32_t initialWidth = 0,
        uint32_t initialHeight = 0)
    {
        constexpr DWORD COMPOSITIONSURFACE_ALL_ACCESS = 0x0003L;

        HANDLE handle = nullptr;
        if (FAILED(DCompositionCreateSurfaceHandle(COMPOSITIONSURFACE_ALL_ACCESS, nullptr, &handle))) {
            OutputDebugStringA("Tab::Create: DCompositionCreateSurfaceHandle FAILED\n");
            return nullptr;
        }

        auto attach = std::make_shared<SwapChainAttachRequest>();
        attach->handle = handle;
        attach->panel = panel;
        attach->dispatcher = panel.DispatcherQueue();
        attach->onActivated = std::move(onActivated);
        // Heap-allocated owning shared_ptr handed to ghostty; it'll come back
        // through OnSwapChainReady (or be deleted here if surface_new fails).
        auto* attachOwned = new std::shared_ptr<SwapChainAttachRequest>(attach);

        ghostty_surface_config_s cfg = ghostty_surface_config_new();
        cfg.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
        cfg.platform.windows.hwnd = hwnd;
        cfg.platform.windows.composition_surface_handle = handle;
        cfg.platform.windows.swap_chain_ready_cb = &OnSwapChainReady;
        cfg.platform.windows.swap_chain_ready_userdata = attachOwned;
        // Initial swap chain size: prefer the host's caller-supplied estimate
        // (typically the active tab's panel size, since the new panel will
        // land in the same TabView content area), then fall back to the
        // panel's own ActualWidth/Height. With deferred SelectedItem (issue
        // #22) the panel isn't in the visual tree yet so its ActualWidth is
        // 0 — without the host hint, ghostty would fall back further to the
        // main window's full client rect, which is taller than the actual
        // panel area by the tab strip height. That mismatch causes a
        // visible "stretch then resize" when the panel becomes visible
        // and SizeChanged fires.
        uint32_t initW = initialWidth ? initialWidth
                                      : static_cast<uint32_t>(panel.ActualWidth());
        uint32_t initH = initialHeight ? initialHeight
                                       : static_cast<uint32_t>(panel.ActualHeight());
        cfg.platform.windows.initial_width = initW;
        cfg.platform.windows.initial_height = initH;
        UINT dpi = GetDpiForWindow(hwnd);
        cfg.scale_factor = static_cast<double>(dpi) / 96.0;

        ghostty_surface_t surface = ghostty_surface_new(app, &cfg);
        if (!surface) {
            OutputDebugStringA("Tab::Create: ghostty_surface_new FAILED\n");
            // Callback won't fire — release the renderer's owning handle.
            delete attachOwned;
            CloseHandle(handle);
            return nullptr;
        }

        try {
            // Private constructor — std::make_unique can't see it, so use new.
            return std::unique_ptr<Tab>(new Tab(std::move(panel), std::move(item), handle, surface, std::move(attach)));
        } catch (winrt::hresult_error const&) {
            // Tab construction failed but the renderer thread may already
            // have fired (or be about to fire) OnSwapChainReady. Cancel
            // first, then tear down what we have.
            attach->cancelled.store(true);
            ghostty_surface_free(surface);
            CloseHandle(handle);
            return nullptr;
        }
    }

private:
    // Private — only Tab::Create can construct. Validates that all resources
    // are present (Create has already checked, so this is a defense-in-depth
    // assertion against future callers inside the class).
    Tab(Microsoft::UI::Xaml::Controls::SwapChainPanel panel,
        Microsoft::UI::Xaml::Controls::TabViewItem item,
        HANDLE surfaceHandle,
        ghostty_surface_t surface,
        std::shared_ptr<SwapChainAttachRequest> attachRequest)
        : m_panel(std::move(panel))
        , m_item(std::move(item))
        , m_surfaceHandle(surfaceHandle)
        , m_surface(surface)
        , m_attachRequest(std::move(attachRequest))
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

    // Renderer-thread callback from ghostty: the swap chain is now bound to
    // our surface handle, so it's safe to call SetSwapChainHandle. We only
    // hop to the UI thread and queue the actual API call — keep this fast.
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

    Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
    Microsoft::UI::Xaml::Controls::TabViewItem m_item{ nullptr };
    HANDLE m_surfaceHandle{ nullptr };
    ghostty_surface_t m_surface{ nullptr };
    winrt::event_token m_sizeChangedToken{};
    std::shared_ptr<SwapChainAttachRequest> m_attachRequest;
};

}  // namespace winrt::GhosttyWin32::implementation
