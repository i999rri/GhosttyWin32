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

            // Get HWND and install input subclass
            auto windowNative = this->try_as<::IWindowNative>();
            if (windowNative) windowNative->get_WindowHandle(&m_hwnd);
            if (m_hwnd) {
                SetWindowSubclass(m_hwnd, InputSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
            }

            auto tv = TabView();
            SetTitleBar(DragRegion());

            tv.AddTabButtonClick([this](muxc::TabView const&, auto&&) {
                CreateTab();
            });

            tv.TabCloseRequested([this](muxc::TabView const& sender, muxc::TabViewTabCloseRequestedEventArgs const& args) {
                uint32_t idx = 0;
                if (sender.TabItems().IndexOf(args.Tab(), idx)) {
                    if (idx < m_sessions.size() && m_sessions[idx]->surface) {
                        ghostty_surface_free(m_sessions[idx]->surface);
                    }
                    if (idx < m_sessions.size() && m_sessions[idx]->swapChain) {
                        m_sessions[idx]->swapChain->Release();
                    }
                    m_sessions.erase(m_sessions.begin() + idx);
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
        if (m_hwnd) RemoveWindowSubclass(m_hwnd, InputSubclass, 1);
        for (auto& s : m_sessions) {
            if (s->surface) ghostty_surface_free(s->surface);
            if (s->swapChain) s->swapChain->Release();
        }
        if (m_app) ghostty_app_free(m_app);
        if (m_config) ghostty_config_free(m_config);
        if (m_d3dDevice) { m_d3dDevice->Release(); m_d3dDevice = nullptr; }
    }

    TabSession* MainWindow::ActiveSession()
    {
        auto tv = TabView();
        auto sel = tv.SelectedItem();
        if (!sel) return nullptr;
        uint32_t idx = 0;
        if (tv.TabItems().IndexOf(sel, idx) && idx < m_sessions.size()) {
            return m_sessions[idx].get();
        }
        return nullptr;
    }

    void MainWindow::InitGhostty()
    {
        UINT flags = 0;
#ifndef NDEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, 1, D3D11_SDK_VERSION, &m_d3dDevice, nullptr, nullptr);

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
        if (hThread) { WaitForSingleObject(hThread, INFINITE); CloseHandle(hThread); }
    }

    void MainWindow::CreateTab()
    {
        if (!m_app || !m_d3dDevice || !m_hwnd) return;
        auto tv = TabView();

        RECT rc;
        GetClientRect(m_hwnd, &rc);
        UINT width = std::max<UINT>(rc.right - rc.left, 1);
        UINT height = std::max<UINT>(rc.bottom - rc.top, 1);

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
        factory->CreateSwapChainForComposition(m_d3dDevice, &scd, nullptr, &swapChain);
        factory->Release(); adapter->Release(); dxgiDevice->Release();
        if (!swapChain) return;

        auto panel = muxc::SwapChainPanel();
        panel.as<ISwapChainPanelNative>()->SetSwapChain(swapChain);

        struct SurfArgs {
            ghostty_app_t app; HWND hwnd;
            ID3D11Device* device; IDXGISwapChain1* sc;
            ghostty_surface_t surface;
        };
        SurfArgs sargs{ m_app, m_hwnd, m_d3dDevice, swapChain, nullptr };

        HANDLE hThread = CreateThread(nullptr, 4 * 1024 * 1024,
            [](LPVOID param) -> DWORD {
                auto* a = static_cast<SurfArgs*>(param);
                ghostty_surface_config_s cfg = ghostty_surface_config_new();
                cfg.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
                cfg.platform.windows.hwnd = a->hwnd;
                cfg.platform.windows.d3d_device = a->device;
                cfg.platform.windows.swap_chain = a->sc;
                UINT dpi = GetDpiForWindow(a->hwnd);
                cfg.scale_factor = (double)dpi / 96.0;
                a->surface = ghostty_surface_new(a->app, &cfg);
                return 0;
            }, &sargs, 0, nullptr);
        if (hThread) { WaitForSingleObject(hThread, INFINITE); CloseHandle(hThread); }

        if (!sargs.surface) { swapChain->Release(); return; }

        auto session = std::make_unique<TabSession>();
        session->surface = sargs.surface;
        session->swapChain = swapChain; // ownership transferred
        session->panel = panel;

        // Input handling via XAML events on SwapChainPanel
        auto surface = sargs.surface;
        auto app = m_app;

        panel.IsTabStop(true);

        panel.CharacterReceived([surface, app](auto&&, winrt::Microsoft::UI::Xaml::Input::CharacterReceivedRoutedEventArgs const& args) {
            wchar_t ch = args.Character();
            if (ch < 0x20) return;
            wchar_t utf16[2] = { ch, 0 };
            char utf8[8] = {};
            int len = WideCharToMultiByte(CP_UTF8, 0, utf16, 1, utf8, sizeof(utf8), nullptr, nullptr);
            if (len > 0) {
                ghostty_surface_text(surface, utf8, len);
                if (app) ghostty_app_tick(app);
                ghostty_surface_refresh(surface);
            }
            args.Handled(true);
        });

        panel.KeyDown([surface](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
            int vk = static_cast<int>(args.Key());
            bool isSpecial = false;
            switch (vk) {
                case VK_BACK: case VK_TAB: case VK_RETURN: case VK_ESCAPE:
                case VK_DELETE: case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
                case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
                case VK_INSERT: case VK_F1: case VK_F2: case VK_F3: case VK_F4:
                case VK_F5: case VK_F6: case VK_F7: case VK_F8: case VK_F9:
                case VK_F10: case VK_F11: case VK_F12:
                case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
                case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
                case VK_MENU: case VK_LMENU: case VK_RMENU:
                    isSpecial = true; break;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && vk >= 'A' && vk <= 'Z')
                isSpecial = true;
            if (!isSpecial) return;

            ghostty_input_key_s keyEvent = {};
            keyEvent.action = GHOSTTY_ACTION_PRESS;
            keyEvent.keycode = args.KeyStatus().ScanCode;
            if (args.KeyStatus().IsExtendedKey) keyEvent.keycode |= 0xE000;
            if (GetKeyState(VK_SHIFT) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_SHIFT);
            if (GetKeyState(VK_CONTROL) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_CTRL);
            if (GetKeyState(VK_MENU) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_ALT);
            ghostty_surface_key(surface, keyEvent);
            args.Handled(true);
        });

        panel.KeyUp([surface](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
            ghostty_input_key_s keyEvent = {};
            keyEvent.action = GHOSTTY_ACTION_RELEASE;
            keyEvent.keycode = args.KeyStatus().ScanCode;
            if (args.KeyStatus().IsExtendedKey) keyEvent.keycode |= 0xE000;
            if (GetKeyState(VK_SHIFT) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_SHIFT);
            if (GetKeyState(VK_CONTROL) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_CTRL);
            if (GetKeyState(VK_MENU) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_ALT);
            ghostty_surface_key(surface, keyEvent);
        });

        // Focus the panel when tab is selected
        panel.Loaded([](auto&& sender, auto&&) {
            sender.as<muxc::SwapChainPanel>().Focus(FocusState::Programmatic);
        });

        m_sessions.push_back(std::move(session));

        auto tab = muxc::TabViewItem();
        tab.Header(box_value(L"Terminal"));
        tab.IsClosable(true);
        tab.Content(panel);
        tv.TabItems().Append(tab);
        tv.SelectedItem(tab);
    }

    LRESULT CALLBACK MainWindow::InputSubclass(HWND hwnd, UINT msg, WPARAM wParam,
        LPARAM lParam, UINT_PTR /*id*/, DWORD_PTR refData)
    {
        auto* self = reinterpret_cast<MainWindow*>(refData);
        auto* sess = self->ActiveSession();

        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
}
