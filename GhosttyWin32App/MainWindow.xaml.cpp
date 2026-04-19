#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include <microsoft.ui.xaml.window.h>
#include <d3d11.h>
#include <dxgi1_2.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace muxc = Microsoft::UI::Xaml::Controls;

namespace winrt::GhosttyWin32::implementation
{
    MainWindow::MainWindow()
    {
        ExtendsContentIntoTitleBar(true);

        Activated([this](auto&&, auto&&) {
            static bool initialized = false;
            if (initialized) return;
            initialized = true;

            auto tv = TabView();
            SetTitleBar(tv);

            tv.AddTabButtonClick([this](muxc::TabView const&, auto&&) {
                CreateTab();
            });

            tv.TabCloseRequested([this](muxc::TabView const& sender, muxc::TabViewTabCloseRequestedEventArgs const& args) {
                uint32_t idx = 0;
                if (sender.TabItems().IndexOf(args.Tab(), idx)) {
                    sender.TabItems().RemoveAt(idx);
                }
                if (sender.TabItems().Size() == 0) {
                    this->Close();
                }
            });

            InitGhostty();
            CreateTab();
        });
    }

    MainWindow::~MainWindow()
    {
        if (m_app) ghostty_app_free(m_app);
        if (m_config) ghostty_config_free(m_config);
        if (m_d3dDevice) { m_d3dDevice->Release(); m_d3dDevice = nullptr; }
    }

    void MainWindow::InitGhostty()
    {
        // Create shared D3D11 device (used by all tabs)
        UINT flags = 0;
#ifndef NDEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL featureLevel;
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, 1, D3D11_SDK_VERSION,
            &m_d3dDevice, &featureLevel, nullptr);
        if (FAILED(hr)) return;

        // Initialize ghostty on 4MB stack thread
        struct Args { MainWindow* self; };
        Args args{ this };
        HANDLE hThread = CreateThread(nullptr, 4 * 1024 * 1024,
            [](LPVOID param) -> DWORD {
                auto* a = static_cast<Args*>(param);
                ghostty_init(0, nullptr);
                ghostty_runtime_config_s rtConfig{};
                rtConfig.wakeup_cb = [](void*) {};
                rtConfig.action_cb = [](ghostty_app_t, ghostty_target_s, ghostty_action_s) -> bool { return false; };
                rtConfig.read_clipboard_cb = [](void*, ghostty_clipboard_e, void*) -> bool { return false; };
                rtConfig.confirm_read_clipboard_cb = [](void*, const char*, void*, ghostty_clipboard_request_e) {};
                rtConfig.write_clipboard_cb = [](void*, ghostty_clipboard_e, const ghostty_clipboard_content_s*, size_t, bool) {};
                rtConfig.close_surface_cb = [](void*, bool) {};
                a->self->m_config = ghostty_config_new();
                ghostty_config_finalize(a->self->m_config);
                a->self->m_app = ghostty_app_new(&rtConfig, a->self->m_config);
                return 0;
            }, &args, 0, nullptr);
        if (hThread) {
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
        }
    }

    void MainWindow::CreateTab()
    {
        if (!m_app || !m_d3dDevice) return;
        auto tv = TabView();

        // Get Window HWND
        auto windowNative = this->try_as<::IWindowNative>();
        HWND hwnd = nullptr;
        if (windowNative) windowNative->get_WindowHandle(&hwnd);
        if (!hwnd) return;

        // Create SwapChainPanel
        auto panel = muxc::SwapChainPanel();

        // Get panel size (use window size as initial)
        RECT rc;
        GetClientRect(hwnd, &rc);
        UINT width = static_cast<UINT>(rc.right - rc.left);
        UINT height = static_cast<UINT>(rc.bottom - rc.top);
        if (width == 0) width = 800;
        if (height == 0) height = 600;

        // Create swap chain for this tab
        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        dxgiDevice->GetAdapter(&adapter);
        adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);

        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width = width;
        scd.Height = height;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.Scaling = DXGI_SCALING_STRETCH;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        IDXGISwapChain1* swapChain = nullptr;
        HRESULT hr = factory->CreateSwapChainForComposition(
            m_d3dDevice, &scd, nullptr, &swapChain);
        factory->Release();
        adapter->Release();
        dxgiDevice->Release();

        if (FAILED(hr) || !swapChain) return;

        // Set swap chain on SwapChainPanel
        auto panelNative = panel.as<ISwapChainPanelNative>();
        panelNative->SetSwapChain(swapChain);

        // Create ghostty surface on 4MB stack thread with external device + swap chain
        struct SurfArgs {
            ghostty_app_t app;
            HWND hwnd;
            ID3D11Device* device;
            IDXGISwapChain1* swapChain;
            ghostty_surface_t surface;
        };
        SurfArgs sargs{ m_app, hwnd, m_d3dDevice, swapChain, nullptr };

        HANDLE hThread = CreateThread(nullptr, 4 * 1024 * 1024,
            [](LPVOID param) -> DWORD {
                auto* a = static_cast<SurfArgs*>(param);
                ghostty_surface_config_s cfg = ghostty_surface_config_new();
                cfg.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
                cfg.platform.windows.hwnd = a->hwnd;
                cfg.platform.windows.d3d_device = a->device;
                cfg.platform.windows.swap_chain = a->swapChain;
                UINT dpi = GetDpiForWindow(a->hwnd);
                cfg.scale_factor = (double)dpi / 96.0;
                a->surface = ghostty_surface_new(a->app, &cfg);
                return 0;
            }, &sargs, 0, nullptr);

        if (hThread) {
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
        }

        // swapChain ownership: ghostty DxDevice AddRef'd it, we release our ref
        swapChain->Release();

        if (!sargs.surface) return;

        // Create tab
        auto tab = muxc::TabViewItem();
        tab.Header(box_value(L"Terminal"));
        tab.IsClosable(true);
        tab.Content(panel);

        tv.TabItems().Append(tab);
        tv.SelectedItem(tab);
    }
}
