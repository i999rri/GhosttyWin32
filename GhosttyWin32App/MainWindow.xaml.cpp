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
                bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
                bool shift = GetKeyState(VK_SHIFT) & 0x8000;

                // Ctrl+C: copy if selection exists, otherwise send SIGINT
                if (ctrl && !shift && vk == 'C') {
                    if (ghostty_surface_has_selection(sess->surface)) {
                        ghostty_text_s text = {};
                        if (ghostty_surface_read_selection(sess->surface, &text) && text.text && text.text_len > 0) {
                            int wlen = MultiByteToWideChar(CP_UTF8, 0, text.text, (int)text.text_len, nullptr, 0);
                            if (wlen > 0 && OpenClipboard(m_hwnd)) {
                                EmptyClipboard();
                                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(wchar_t));
                                if (hMem) {
                                    wchar_t* dest = static_cast<wchar_t*>(GlobalLock(hMem));
                                    MultiByteToWideChar(CP_UTF8, 0, text.text, (int)text.text_len, dest, wlen);
                                    dest[wlen] = L'\0';
                                    GlobalUnlock(hMem);
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                }
                                CloseClipboard();
                            }
                            ghostty_surface_free_text(sess->surface, &text);
                        }
                        // Clear selection
                        ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                        ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                        args.Handled(true);
                        return;
                    }
                }

                // Ctrl+V: paste from clipboard
                if (ctrl && !shift && vk == 'V') {
                    if (OpenClipboard(m_hwnd)) {
                        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                        if (hData) {
                            wchar_t* wstr = static_cast<wchar_t*>(GlobalLock(hData));
                            if (wstr) {
                                int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
                                if (len > 1) {
                                    std::string utf8(len - 1, '\0');
                                    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8.data(), len, nullptr, nullptr);
                                    ghostty_surface_text(sess->surface, utf8.c_str(), utf8.size());
                                }
                                GlobalUnlock(hData);
                            }
                        }
                        CloseClipboard();
                    }
                    if (m_app) ghostty_app_tick(m_app);
                    ghostty_surface_refresh(sess->surface);
                    args.Handled(true);
                    return;
                }

                // Send key event to ghostty
                ghostty_input_key_s keyEvent = {};
                keyEvent.action = GHOSTTY_ACTION_PRESS;
                keyEvent.keycode = scanCode;
                if (args.KeyStatus().IsExtendedKey) keyEvent.keycode |= 0xE000;
                if (shift) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_SHIFT);
                if (ctrl) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_CTRL);
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
                ghostty_input_mouse_button_e btn;
                if (props.IsLeftButtonPressed()) btn = GHOSTTY_MOUSE_LEFT;
                else if (props.IsRightButtonPressed()) {
                    // Right-click: copy selection if exists
                    if (ghostty_surface_has_selection(sess->surface)) {
                        ghostty_text_s text = {};
                        if (ghostty_surface_read_selection(sess->surface, &text) && text.text && text.text_len > 0) {
                            int wlen = MultiByteToWideChar(CP_UTF8, 0, text.text, (int)text.text_len, nullptr, 0);
                            if (wlen > 0 && OpenClipboard(m_hwnd)) {
                                EmptyClipboard();
                                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(wchar_t));
                                if (hMem) {
                                    wchar_t* dest = static_cast<wchar_t*>(GlobalLock(hMem));
                                    MultiByteToWideChar(CP_UTF8, 0, text.text, (int)text.text_len, dest, wlen);
                                    dest[wlen] = L'\0';
                                    GlobalUnlock(hMem);
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                }
                                CloseClipboard();
                            }
                            ghostty_surface_free_text(sess->surface, &text);
                        }
                        // Clear selection
                        ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                        ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                        return;
                    }
                    btn = GHOSTTY_MOUSE_RIGHT;
                }
                else return; // Ignore middle-click and others
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
                if (sender.TabItems().IndexOf(args.Tab(), idx) && idx < m_sessions.size()) {
                    if (m_sessions[idx]->surface) ghostty_surface_free(m_sessions[idx]->surface);
                    if (m_sessions[idx]->swapChain) m_sessions[idx]->swapChain->Release();
                    if (m_sessions[idx]->device) m_sessions[idx]->device->Release();
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
            if (s->device) s->device->Release();
        }
        if (m_app) ghostty_app_free(m_app);
        if (m_config) ghostty_config_free(m_config);
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

        struct Args { MainWindow* self; };
        Args args{ this };
        HANDLE hThread = CreateThread(nullptr, 4 * 1024 * 1024,
            [](LPVOID param) -> DWORD {
                auto* a = static_cast<Args*>(param);
                ghostty_init(0, nullptr);
                ghostty_runtime_config_s rtConfig{};
                rtConfig.userdata = a->self;
                rtConfig.wakeup_cb = [](void*) {
                    if (!g_mainWindow || !g_mainWindow->m_app) return;
                    g_mainWindow->DispatcherQueue().TryEnqueue([]() {
                        if (g_mainWindow && g_mainWindow->m_app) {
                            ghostty_app_tick(g_mainWindow->m_app);
                        }
                    });
                };
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
                rtConfig.read_clipboard_cb = [](void*, ghostty_clipboard_e, void* state) -> bool {
                    if (!g_mainWindow) return false;
                    auto* sess = g_mainWindow->ActiveSession();
                    if (!sess || !sess->surface) return false;
                    if (!OpenClipboard(g_mainWindow->m_hwnd)) return false;
                    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                    if (!hData) { CloseClipboard(); return false; }
                    wchar_t* wstr = static_cast<wchar_t*>(GlobalLock(hData));
                    if (!wstr) { CloseClipboard(); return false; }
                    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 0) {
                        std::string utf8(len - 1, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8.data(), len, nullptr, nullptr);
                        GlobalUnlock(hData);
                        CloseClipboard();
                        ghostty_surface_complete_clipboard_request(sess->surface, utf8.c_str(), state, false);
                        return true;
                    }
                    GlobalUnlock(hData);
                    CloseClipboard();
                    return false;
                };
                rtConfig.confirm_read_clipboard_cb = [](void*, const char* content, void* state, ghostty_clipboard_request_e) {
                    // Auto-confirm clipboard reads
                    if (g_mainWindow) {
                        auto* sess = g_mainWindow->ActiveSession();
                        if (sess && sess->surface) {
                            ghostty_surface_complete_clipboard_request(sess->surface, content, state, true);
                        }
                    }
                };
                rtConfig.write_clipboard_cb = [](void*, ghostty_clipboard_e, const ghostty_clipboard_content_s* content, size_t count, bool) {
                    if (!content || count == 0) return;
                    const char* text = content[0].data;
                    if (!text) return;
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
                    if (wlen <= 0) return;
                    HWND hwnd = g_mainWindow ? g_mainWindow->m_hwnd : nullptr;
                    if (OpenClipboard(hwnd)) {
                        EmptyClipboard();
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
                        if (hMem) {
                            wchar_t* dest = static_cast<wchar_t*>(GlobalLock(hMem));
                            MultiByteToWideChar(CP_UTF8, 0, text, -1, dest, wlen);
                            GlobalUnlock(hMem);
                            SetClipboardData(CF_UNICODETEXT, hMem);
                        }
                        CloseClipboard();
                    }
                };
                // TODO: ghostty doesn't call close_surface_cb on shell exit (see ghostty#34)
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
        if (!m_app || !m_hwnd) return;
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
        // Hide panel until surface is ready to avoid black flash
        if (sessionIdx > 0) panel.Opacity(0);

        auto app = m_app;
        auto hwnd = m_hwnd;
        auto weakThis = get_weak();

        panel.Loaded([sessionIdx, app, hwnd, weakThis](auto&& sender, auto&&) {

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

            // Create a D3D11 device per tab — each surface needs its own
            // device+context to avoid multithreading conflicts
            ID3D11Device* device = nullptr;
            UINT flags = 0;
#ifndef NDEBUG
            flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
            D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                levels, 1, D3D11_SDK_VERSION, &device, nullptr, nullptr);
            if (!device) return;
            sess->device = device;

            // Create swap chain and link to panel
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

            p.template as<ISwapChainPanelNative>()->SetSwapChain(swapChain);
            sess->swapChain = swapChain;


            // Async surface creation — don't block UI thread
            struct SurfContext {
                TabSession* sess;
                ghostty_app_t app;
                ID3D11Device* device;
                IDXGISwapChain1* swapChain;
                HWND hwnd;
                ghostty_surface_t surface;
            };
            auto ctx = new SurfContext{ sess, app, device, swapChain, hwnd, nullptr };

            CreateThread(nullptr, 4 * 1024 * 1024,
                [](LPVOID param) -> DWORD {
                    auto* c = static_cast<SurfContext*>(param);
                    ghostty_surface_config_s cfg = ghostty_surface_config_new();
                    cfg.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
                    cfg.platform.windows.hwnd = c->hwnd;
                    cfg.platform.windows.d3d_device = c->device;
                    cfg.platform.windows.swap_chain = c->swapChain;
                    UINT dpi = GetDpiForWindow(c->hwnd);
                    cfg.scale_factor = (double)dpi / 96.0;
                    c->surface = ghostty_surface_new(c->app, &cfg);
                    return 0;
                }, ctx, 0, nullptr);

            // Poll for surface creation completion without blocking UI
            auto pollTimer = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
            pollTimer.Interval(std::chrono::milliseconds(16));
            pollTimer.IsRepeating(true);
            pollTimer.Tick([ctx, pollTimer, hwnd](auto&&, auto&&) {
                if (!ctx->surface) return;
                pollTimer.Stop();

                ctx->sess->surface = ctx->surface;
                ShowWindow(hwnd, SW_SHOW);
                ctx->sess->panel.Opacity(1);

                auto surface = ctx->surface;
                auto panel = ctx->sess->panel;
                panel.SizeChanged([surface](auto&&, winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args) {
                    auto newSize = args.NewSize();
                    uint32_t w = static_cast<uint32_t>(newSize.Width);
                    uint32_t h = static_cast<uint32_t>(newSize.Height);
                    if (w > 0 && h > 0) {
                        ghostty_surface_set_size(surface, w, h);
                    }
                });

                delete ctx;
            });
            pollTimer.Start();
        });
    }
}
