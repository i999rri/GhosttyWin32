#pragma once

#include "ghostty.h"
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <dcomp.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#pragma comment(lib, "dcomp.lib")

namespace winrt::GhosttyWin32::implementation {

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

    // Factory: given an already-loaded panel + item, do the failable
    // orchestration (handle creation, panel attachment, ghostty surface
    // creation) and return a fully-formed Tab. Returns nullptr on failure
    // (after cleaning up any partially-acquired resources). Must be called
    // on the UI thread inside panel.Loaded so SetSwapChainHandle has a
    // valid composition tree.
    static std::unique_ptr<Tab> Create(
        Microsoft::UI::Xaml::Controls::SwapChainPanel panel,
        Microsoft::UI::Xaml::Controls::TabViewItem item,
        ghostty_app_t app,
        HWND hwnd)
    {
        constexpr DWORD COMPOSITIONSURFACE_ALL_ACCESS = 0x0003L;

        HANDLE handle = nullptr;
        if (FAILED(DCompositionCreateSurfaceHandle(COMPOSITIONSURFACE_ALL_ACCESS, nullptr, &handle))) {
            OutputDebugStringA("Tab::Create: DCompositionCreateSurfaceHandle FAILED\n");
            return nullptr;
        }

        auto native2 = panel.try_as<ISwapChainPanelNative2>();
        if (!native2 || FAILED(native2->SetSwapChainHandle(handle))) {
            OutputDebugStringA("Tab::Create: SetSwapChainHandle FAILED\n");
            CloseHandle(handle);
            return nullptr;
        }

        ghostty_surface_config_s cfg = ghostty_surface_config_new();
        cfg.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
        cfg.platform.windows.hwnd = hwnd;
        cfg.platform.windows.composition_surface_handle = handle;
        // Pass the panel's actual layout size so ghostty creates the swap
        // chain at the final size — saves an immediate ResizeBuffers on the
        // first frame and reduces driver allocator stress over many tabs.
        cfg.platform.windows.initial_width = static_cast<uint32_t>(panel.ActualWidth());
        cfg.platform.windows.initial_height = static_cast<uint32_t>(panel.ActualHeight());
        UINT dpi = GetDpiForWindow(hwnd);
        cfg.scale_factor = static_cast<double>(dpi) / 96.0;

        ghostty_surface_t surface = ghostty_surface_new(app, &cfg);
        if (!surface) {
            OutputDebugStringA("Tab::Create: ghostty_surface_new FAILED\n");
            native2->SetSwapChainHandle(nullptr);
            CloseHandle(handle);
            return nullptr;
        }

        try {
            // Private constructor — std::make_unique can't see it, so use new.
            return std::unique_ptr<Tab>(new Tab(std::move(panel), std::move(item), handle, surface));
        } catch (winrt::hresult_error const&) {
            ghostty_surface_free(surface);
            native2->SetSwapChainHandle(nullptr);
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
        ghostty_surface_t surface)
        : m_panel(std::move(panel))
        , m_item(std::move(item))
        , m_surfaceHandle(surfaceHandle)
        , m_surface(surface)
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

    Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
    Microsoft::UI::Xaml::Controls::TabViewItem m_item{ nullptr };
    HANDLE m_surfaceHandle{ nullptr };
    ghostty_surface_t m_surface{ nullptr };
    winrt::event_token m_sizeChangedToken{};
};

}  // namespace winrt::GhosttyWin32::implementation
