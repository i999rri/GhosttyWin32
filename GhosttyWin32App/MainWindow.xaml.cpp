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

static winrt::GhosttyWin32::implementation::MainWindow* g_mainWindow = nullptr;

namespace winrt::GhosttyWin32::implementation
{
    MainWindow::MainWindow()
    {
        ExtendsContentIntoTitleBar(true);

        Activated([this](auto&&, auto&&) {
            static bool initialized = false;
            if (initialized) return;
            initialized = true;

            g_mainWindow = this;
            auto windowNative = this->try_as<::IWindowNative>();
            if (windowNative) windowNative->get_WindowHandle(&m_hwnd);
            if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);

            auto tv = TabView();
            SetTitleBar(DragRegion());

            // Window-level input handling (same approach as Windows Terminal)
            auto root = Content().as<winrt::Microsoft::UI::Xaml::UIElement>();

            root.KeyDown([this](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                auto* sess = ActiveSession();
                if (!sess || !sess->surface) return;

                int vk = static_cast<int>(args.Key());
                UINT scanCode = args.KeyStatus().ScanCode;

                // Send key event to ghostty
                ghostty_input_key_s keyEvent = {};
                keyEvent.action = GHOSTTY_ACTION_PRESS;
                keyEvent.keycode = scanCode;
                if (args.KeyStatus().IsExtendedKey) keyEvent.keycode |= 0xE000;
                if (GetKeyState(VK_SHIFT) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_SHIFT);
                if (GetKeyState(VK_CONTROL) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_CTRL);
                if (GetKeyState(VK_MENU) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_ALT);
                ghostty_surface_key(sess->surface, keyEvent);

                // Translate to text using ToUnicode (replaces CharacterReceived)
                BYTE kbState[256] = {};
                GetKeyboardState(kbState);
                wchar_t chars[4] = {};
                int charCount = ToUnicode(vk, scanCode, kbState, chars, 4, 0);
                if (charCount > 0 && chars[0] >= 0x20) {
                    char utf8[16] = {};
                    int len = WideCharToMultiByte(CP_UTF8, 0, chars, charCount, utf8, sizeof(utf8), nullptr, nullptr);
                    if (len > 0) {
                        ghostty_surface_text(sess->surface, utf8, len);
                    }
                }

                if (m_app) ghostty_app_tick(m_app);
                ghostty_surface_refresh(sess->surface);
                args.Handled(true);
            });

            // Mouse input on root
            root.PointerMoved([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                auto* sess = ActiveSession();
                if (!sess || !sess->surface) return;
                winrt::Microsoft::UI::Input::PointerPoint point = args.GetCurrentPoint(sess->panel);
                winrt::Windows::Foundation::Point pos = point.Position();
                ghostty_input_mods_e mods = (ghostty_input_mods_e)0;
                if (GetKeyState(VK_SHIFT) & 0x8000) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_SHIFT);
                if (GetKeyState(VK_CONTROL) & 0x8000) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_CTRL);
                if (GetKeyState(VK_MENU) & 0x8000) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_ALT);
                ghostty_surface_mouse_pos(sess->surface, pos.X, pos.Y, mods);
            });

            root.PointerPressed([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                auto* sess = ActiveSession();
                if (!sess || !sess->surface) return;
                winrt::Microsoft::UI::Input::PointerPoint point = args.GetCurrentPoint(sess->panel);
                winrt::Microsoft::UI::Input::PointerPointProperties props = point.Properties();
                ghostty_input_mouse_button_e btn = GHOSTTY_MOUSE_LEFT;
                if (props.IsRightButtonPressed()) btn = GHOSTTY_MOUSE_RIGHT;
                else if (props.IsMiddleButtonPressed()) btn = GHOSTTY_MOUSE_MIDDLE;
                ghostty_input_mods_e mods = (ghostty_input_mods_e)0;
                if (GetKeyState(VK_SHIFT) & 0x8000) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_SHIFT);
                if (GetKeyState(VK_CONTROL) & 0x8000) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_CTRL);
                if (GetKeyState(VK_MENU) & 0x8000) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_ALT);
                ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_PRESS, btn, mods);
            });

            root.PointerReleased([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                auto* sess = ActiveSession();
                if (!sess || !sess->surface) return;
                ghostty_input_mods_e mods = (ghostty_input_mods_e)0;
                if (GetKeyState(VK_SHIFT) & 0x8000) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_SHIFT);
                if (GetKeyState(VK_CONTROL) & 0x8000) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_CTRL);
                if (GetKeyState(VK_MENU) & 0x8000) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_ALT);
                ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, mods);
            });

            root.PointerWheelChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                auto* sess = ActiveSession();
                if (!sess || !sess->surface) return;
                winrt::Microsoft::UI::Input::PointerPoint point = args.GetCurrentPoint(sess->panel);
                winrt::Microsoft::UI::Input::PointerPointProperties props = point.Properties();
                int delta = props.MouseWheelDelta();
                double scrollY = (double)delta / 120.0;
                ghostty_input_scroll_mods_t smods = {};
                ghostty_surface_mouse_scroll(sess->surface, 0, scrollY, smods);
                args.Handled(true);
            });

            root.KeyUp([this](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                auto* sess = ActiveSession();
                if (!sess || !sess->surface) return;
                ghostty_input_key_s keyEvent = {};
                keyEvent.action = GHOSTTY_ACTION_RELEASE;
                keyEvent.keycode = args.KeyStatus().ScanCode;
                if (args.KeyStatus().IsExtendedKey) keyEvent.keycode |= 0xE000;
                if (GetKeyState(VK_SHIFT) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_SHIFT);
                if (GetKeyState(VK_CONTROL) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_CTRL);
                if (GetKeyState(VK_MENU) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_ALT);
                ghostty_surface_key(sess->surface, keyEvent);
            });

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
                rtConfig.userdata = a->self;
                rtConfig.wakeup_cb = [](void*) {};
                rtConfig.action_cb = [](ghostty_app_t, ghostty_target_s target, ghostty_action_s action) -> bool {
                    if ((action.tag == GHOSTTY_ACTION_SET_TITLE || action.tag == GHOSTTY_ACTION_SET_TAB_TITLE)
                        && target.tag == GHOSTTY_TARGET_SURFACE) {
                        const char* title = action.action.set_title.title;
                        auto surface = target.target.surface;
                        if (title && g_mainWindow) {
                            int wlen = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
                            if (wlen > 0) {
                                auto wstr = std::make_shared<std::wstring>(wlen - 1, L'\0');
                                MultiByteToWideChar(CP_UTF8, 0, title, -1, wstr->data(), wlen);
                                auto mw = g_mainWindow;
                                mw->DispatcherQueue().TryEnqueue([mw, wstr, surface]() {
                                    auto tv = mw->TabView();
                                    for (uint32_t i = 0; i < tv.TabItems().Size() && i < mw->m_sessions.size(); i++) {
                                        if (mw->m_sessions[i]->surface == surface) {
                                            auto tab = tv.TabItems().GetAt(i).as<muxc::TabViewItem>();
                                            tab.Header(box_value(winrt::hstring(*wstr)));
                                            break;
                                        }
                                    }
                                });
                            }
                        }
                    }
                    return false;
                };
                rtConfig.read_clipboard_cb = [](void*, ghostty_clipboard_e, void*) -> bool { return false; };
                rtConfig.confirm_read_clipboard_cb = [](void*, const char*, void*, ghostty_clipboard_request_e) {};
                rtConfig.write_clipboard_cb = [](void*, ghostty_clipboard_e, const ghostty_clipboard_content_s*, size_t, bool) {};
                rtConfig.close_surface_cb = [](void*, bool) {};
                a->self->m_config = ghostty_config_new();
                ghostty_config_load_default_files(a->self->m_config);
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

        auto panel = muxc::SwapChainPanel();
        panel.IsTabStop(true);
        panel.IsHitTestVisible(true);
        panel.AllowFocusOnInteraction(true);

        auto session = std::make_unique<TabSession>();
        session->panel = panel;
        size_t sessionIdx = m_sessions.size();
        m_sessions.push_back(std::move(session));

        auto tab = muxc::TabViewItem();
        tab.Header(box_value(L"Terminal"));
        tab.IsClosable(true);
        tab.Content(panel);
        tv.TabItems().Append(tab);
        tv.SelectedItem(tab);

        // Defer surface creation until panel is in the visual tree
        auto app = m_app;
        auto device = m_d3dDevice;
        auto hwnd = m_hwnd;
        auto weakThis = get_weak();

        panel.Loaded([sessionIdx, app, device, hwnd, weakThis](auto&& sender, auto&&) {

            auto self = weakThis.get();
            if (!self) return;
            if (sessionIdx >= self->m_sessions.size()) return;
            auto* sess = self->m_sessions[sessionIdx].get();
            if (sess->surface) return;

            auto p = sender.as<muxc::SwapChainPanel>();

            RECT rc;
            GetClientRect(hwnd, &rc);
            UINT width = std::max<UINT>(rc.right - rc.left, 1);
            UINT height = std::max<UINT>(rc.bottom - rc.top, 1);

            // Create swap chain and link to panel on UI thread
            IDXGIDevice* dxgiDev = nullptr;
            IDXGIAdapter* adapter = nullptr;
            IDXGIFactory2* factory = nullptr;
            device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
            dxgiDev->GetAdapter(&adapter);
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
            factory->CreateSwapChainForComposition(device, &scd, nullptr, &swapChain);
            factory->Release(); adapter->Release(); dxgiDev->Release();
            if (!swapChain) return;

            p.as<ISwapChainPanelNative>()->SetSwapChain(swapChain);
            sess->swapChain = swapChain;


            // Delay surface creation by 100ms to let composition engine
            // fully register the swap chain with the NVIDIA driver
            auto timer = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
            timer.Interval(std::chrono::milliseconds(100));
            timer.IsRepeating(false);
            timer.Tick([sess, app, device, swapChain, hwnd, timer](auto&&, auto&&) {

                struct SurfArgs {
                    ghostty_app_t app; HWND hwnd;
                    ID3D11Device* device; IDXGISwapChain1* sc;
                    ghostty_surface_t surface;
                };
                SurfArgs sargs{ app, hwnd, device, swapChain, nullptr };

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

                if (sargs.surface) {
                    sess->surface = sargs.surface;
                    ShowWindow(hwnd, SW_SHOW);


                    // Resize handler on the SwapChainPanel
                    auto surface = sargs.surface;
                    auto panel = sess->panel;
                    panel.SizeChanged([surface](auto&&, winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args) {
                        auto newSize = args.NewSize();
                        uint32_t w = static_cast<uint32_t>(newSize.Width);
                        uint32_t h = static_cast<uint32_t>(newSize.Height);
                        if (w > 0 && h > 0) {
                            ghostty_surface_set_size(surface, w, h);
                        }
                    });
                } else {
                }
            });
            timer.Start();
        });
    }
}
