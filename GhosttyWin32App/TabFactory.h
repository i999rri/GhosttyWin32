#pragma once

#include "Tab.h"
#include "TabId.h"
#include "TabIdAllocator.h"
#include "TerminalControl.xaml.h"
#include "ghostty.h"
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <dcomp.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <functional>
#include <memory>

#pragma comment(lib, "dcomp.lib")

namespace winrt::GhosttyWin32::implementation {

// Builds Tabs. Holds the cross-cutting context (ghostty app handle, the
// HWND for DPI/initial-size, and the TabIdAllocator that produces fresh
// IDs) so callers don't have to thread those through every Make() call.
//
// Stateless beyond the injected references — no mutable state of its
// own. ID counter mutation lives in TabIdAllocator; the factory only
// borrows it.
class TabFactory {
public:
    TabFactory(ghostty_app_t app, HWND hwnd, TabIdAllocator& idAllocator) noexcept
        : m_app(app), m_hwnd(hwnd), m_idAllocator(idAllocator) {}

    TabFactory(const TabFactory&) = delete;
    TabFactory& operator=(const TabFactory&) = delete;
    TabFactory(TabFactory&&) = delete;
    TabFactory& operator=(TabFactory&&) = delete;

    // Build a fully-formed Tab from a pre-created TerminalControl + item.
    // The caller is expected to have already wired the control into the
    // visual tree (item.Content(control) + tv.TabItems().Append(item))
    // so the inner panel's DispatcherQueue is reachable.
    //
    // Returns nullptr on failure (after cleaning up any partially-
    // acquired resources). Call on the UI thread; the panel does NOT
    // need to be in the visual tree yet — see issue #22, where making
    // the panel visible before it had displayable content produced a
    // flicker. The optional onActivated callback runs on the UI thread
    // once ghostty has presented its first frame and we've bound the
    // swap chain to the panel; the host uses it to switch the TabView
    // so the panel becomes visible only with real content.
    //
    // Ordering: the DComp surface handle is bound to the panel only
    // AFTER ghostty's renderer thread has presented at least one real
    // frame — see TerminalControl::OnSwapChainReady. ghostty fires the
    // swap-chain-ready callback from drawFrameEnd (post first present),
    // not from swap-chain creation, so the back buffer is guaranteed to
    // have displayable content by the time we attach.
    std::unique_ptr<Tab> Make(
        winrt::GhosttyWin32::TerminalControl control,
        Microsoft::UI::Xaml::Controls::TabViewItem item,
        std::function<void()> onActivated = {},
        uint32_t initialWidth = 0,
        uint32_t initialHeight = 0)
    {
        constexpr DWORD COMPOSITIONSURFACE_ALL_ACCESS = 0x0003L;

        auto* controlImpl = winrt::get_self<implementation::TerminalControl>(control);
        if (!controlImpl) {
            OutputDebugStringA("TabFactory::Make: get_self<TerminalControl> FAILED\n");
            return nullptr;
        }
        auto panel = controlImpl->InnerPanel();
        if (!panel) {
            OutputDebugStringA("TabFactory::Make: TerminalControl has no inner panel\n");
            return nullptr;
        }

        HANDLE handle = nullptr;
        if (FAILED(DCompositionCreateSurfaceHandle(COMPOSITIONSURFACE_ALL_ACCESS, nullptr, &handle))) {
            OutputDebugStringA("TabFactory::Make: DCompositionCreateSurfaceHandle FAILED\n");
            return nullptr;
        }

        auto attach = std::make_shared<SwapChainAttachRequest>();
        attach->handle = handle;
        attach->panel = panel;
        attach->dispatcher = panel.DispatcherQueue();
        attach->onActivated = std::move(onActivated);
        // Heap-allocated owning shared_ptr handed to ghostty; it'll
        // come back through OnSwapChainReady (or be deleted here if
        // surface_new fails).
        auto* attachOwned = new std::shared_ptr<SwapChainAttachRequest>(attach);

        // ID for the close-surface callback path. Allocated before
        // surface_new because cfg.userdata must be set up-front; the
        // value is opaque to ghostty and travels back to us through
        // close_surface_cb.
        TabId id = m_idAllocator.Allocate();

        ghostty_surface_config_s cfg = ghostty_surface_config_new();
        cfg.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
        cfg.platform.windows.hwnd = m_hwnd;
        cfg.platform.windows.composition_surface_handle = handle;
        cfg.platform.windows.swap_chain_ready_cb = &TerminalControl::OnSwapChainReady;
        cfg.platform.windows.swap_chain_ready_userdata = attachOwned;
        cfg.userdata = id.ToUserdata();
        // Initial swap chain size: prefer the host's caller-supplied
        // estimate (typically the active tab's panel size, since the
        // new panel will land in the same TabView content area), then
        // fall back to the panel's own ActualWidth/Height. With
        // deferred SelectedItem (issue #22) the panel isn't in the
        // visual tree yet so its ActualWidth is 0 — without the host
        // hint, ghostty would fall back further to the main window's
        // full client rect, which is taller than the actual panel area
        // by the tab strip height. That mismatch causes a visible
        // "stretch then resize" when the panel becomes visible and
        // SizeChanged fires.
        uint32_t initW = initialWidth ? initialWidth
                                      : static_cast<uint32_t>(panel.ActualWidth());
        uint32_t initH = initialHeight ? initialHeight
                                       : static_cast<uint32_t>(panel.ActualHeight());
        cfg.platform.windows.initial_width = initW;
        cfg.platform.windows.initial_height = initH;
        UINT dpi = GetDpiForWindow(m_hwnd);
        cfg.scale_factor = static_cast<double>(dpi) / 96.0;

        ghostty_surface_t surface = ghostty_surface_new(m_app, &cfg);
        if (!surface) {
            OutputDebugStringA("TabFactory::Make: ghostty_surface_new FAILED\n");
            // Callback won't fire — release the renderer's owning handle.
            delete attachOwned;
            CloseHandle(handle);
            return nullptr;
        }

        // Hand surface ownership to the control. From here on the
        // control's Detach() (called from Tab::~Tab) is responsible for
        // freeing the surface and closing the handle. The app handle
        // is needed inside the control to drive ghostty_app_tick after
        // IME / keyboard input commits.
        controlImpl->Attach(m_app, surface, handle, m_hwnd, attach);

        try {
            return std::make_unique<Tab>(std::move(control), std::move(item), id);
        } catch (winrt::hresult_error const&) {
            // Tab construction validation failed. Detach synchronously
            // so the surface/handle don't leak.
            controlImpl->Detach();
            return nullptr;
        }
    }

private:
    ghostty_app_t m_app;
    HWND m_hwnd;
    TabIdAllocator& m_idAllocator;
};

}  // namespace winrt::GhosttyWin32::implementation
