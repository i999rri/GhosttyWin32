#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Input.h>
#include <dwmapi.h>
#include <shellapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Input;

extern "C" void dx_notify_resize(uint32_t w, uint32_t h);
extern "C" int dx_set_panel_swap_chain(void* swap_chain_panel);

namespace winrt::GhosttyWin32::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();

        // Initialize ghostty after first activation
        this->Activated([this](auto&&, auto&&) {
            static bool initialized = false;
            if (!initialized) {
                initialized = true;
                InitializeTerminal();
            }
        });
    }

    MainWindow::~MainWindow()
    {
        if (m_tsfInput) {
            m_tsfInput->Unfocus();
            m_tsfInput->Uninitialize();
            m_tsfInput->Release();
            m_tsfInput = nullptr;
        }
        if (m_surface) {
            m_ghosttyApp.destroySurface(m_surface);
            m_surface = nullptr;
        }
        if (m_panelNative) {
            m_panelNative->Release();
            m_panelNative = nullptr;
        }
    }

    HWND MainWindow::GetWindowHandle()
    {
        auto windowNative = this->try_as<::IWindowNative>();
        if (!windowNative) return nullptr;
        HWND hwnd = nullptr;
        windowNative->get_WindowHandle(&hwnd);
        return hwnd;
    }

    void* MainWindow::GetSwapChainPanelNative()
    {
        auto panel = TerminalPanel();
        auto panelInterop = panel.as<ISwapChainPanelNative>();
        if (!panelInterop) return nullptr;

        // AddRef so we can hold onto it
        ISwapChainPanelNative* raw = panelInterop.get();
        raw->AddRef();
        m_panelNative = raw;
        return raw;
    }

    void MainWindow::InitializeTerminal()
    {
        if (!m_ghosttyApp.initialize()) {
            OutputDebugStringA("ghostty: initialization failed\n");
            return;
        }

        HWND mainHwnd = GetWindowHandle();

        // Set callbacks
        m_ghosttyApp.onWakeupFn = [mainHwnd]() {
            if (mainHwnd) PostMessageW(mainHwnd, WM_USER + 1, 0, 0);
        };

        m_ghosttyApp.onActionFn = [this](ghostty_target_s target, ghostty_action_s action) {
            return HandleAction(target, action);
        };

        // Get ISwapChainPanelNative for composition mode
        void* panelNative = GetSwapChainPanelNative();

        // Create surface with SwapChainPanel
        // ghostty will call CreateSwapChainForComposition + SetSwapChain on the panel
        ghostty_surface_config_s surfConfig = ghostty_surface_config_new();
        surfConfig.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
        surfConfig.platform.windows.hwnd = mainHwnd;
        surfConfig.platform.windows.hdc = nullptr;
        surfConfig.platform.windows.hglrc = nullptr;
        surfConfig.platform.windows.swap_chain_panel = panelNative;

        UINT dpi = GetDpiForWindow(mainHwnd);
        surfConfig.scale_factor = (double)dpi / 96.0;

        // Create surface on 4MB stack thread
        struct Args {
            GhosttyApp* app;
            ghostty_surface_config_s* config;
            ghostty_surface_t result;
        };
        Args args = { &m_ghosttyApp, &surfConfig, nullptr };

        HANDLE hThread = CreateThread(
            nullptr, 4 * 1024 * 1024,
            [](LPVOID param) -> DWORD {
                auto* a = static_cast<Args*>(param);
                a->result = ghostty_surface_new(a->app->app(), a->config);
                return 0;
            },
            &args, 0, nullptr);

        if (hThread) {
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
        }

        m_surface = args.result;
        if (!m_surface) {
            OutputDebugStringA("ghostty: surface creation failed\n");
            return;
        }

        OutputDebugStringA("ghostty: surface created with SwapChainPanel!\n");

        // Wait for renderer thread to create D3D11 device, then set swap chain on panel (UI thread)
        // dx_set_panel_swap_chain returns 0 on success, -1 if device not ready yet
        {
            void* panel = panelNative;
            int retries = 0;
            while (retries < 100) { // up to ~1 second
                int result = dx_set_panel_swap_chain(panel);
                if (result == 0) {
                    OutputDebugStringA("ghostty: SetSwapChain on panel succeeded!\n");
                    break;
                }
                Sleep(10);
                retries++;
                // Pump messages so XAML stays responsive
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            if (retries >= 100) {
                OutputDebugStringA("ghostty: SetSwapChain timed out waiting for device\n");
            }
        }

        // Initialize TSF for IME input (IME switching not yet working on WinUI 3)
        m_tsfInput = new TsfInput();
        {
            auto surface = m_surface;
            auto app = m_ghosttyApp.app();
            TsfInput::Callbacks tsfCallbacks;
            tsfCallbacks.getHwnd = [mainHwnd]() { return mainHwnd; };
            tsfCallbacks.getViewportRect = [mainHwnd]() -> RECT {
                RECT rc; GetWindowRect(mainHwnd, &rc); return rc;
            };
            tsfCallbacks.getCursorRect = [mainHwnd, surface]() -> RECT {
                double x = 0, y = 0, w = 0, h = 0;
                if (surface) ghostty_surface_ime_point(surface, &x, &y, &w, &h);
                POINT pt = { (LONG)x, (LONG)(y + h) };
                ClientToScreen(mainHwnd, &pt);
                return { pt.x, pt.y, pt.x + (LONG)w, pt.y + (LONG)h };
            };
            tsfCallbacks.handleOutput = [surface, app](std::wstring_view text) {
                if (!surface || text.empty()) return;
                char utf8[256] = {};
                int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                    utf8, sizeof(utf8), nullptr, nullptr);
                if (len > 0) {
                    ghostty_surface_text(surface, utf8, len);
                    if (app) ghostty_app_tick(app);
                    ghostty_surface_refresh(surface);
                }
            };
            tsfCallbacks.handleComposition = [surface](std::wstring_view text) {
                if (!surface) return;
                if (text.empty()) {
                    ghostty_surface_preedit(surface, nullptr, 0);
                } else {
                    char utf8[256] = {};
                    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
                        utf8, sizeof(utf8), nullptr, nullptr);
                    if (len > 0) ghostty_surface_preedit(surface, utf8, len);
                }
            };
            if (!m_tsfInput->Initialize(std::move(tsfCallbacks))) {
                OutputDebugStringA("ghostty: TSF initialization failed\n");
            } else {
                m_tsfInput->Focus();
            }
        }

        // Register input events — key events on window content level
        auto panel = TerminalPanel();
        this->Content().KeyDown({ this, &MainWindow::OnTerminalKeyDown });

        // Pointer events on the SwapChainPanel
        panel.PointerMoved({ this, &MainWindow::OnTerminalPointerMoved });
        panel.PointerPressed({ this, &MainWindow::OnTerminalPointerPressed });
        panel.PointerReleased({ this, &MainWindow::OnTerminalPointerReleased });
        panel.PointerWheelChanged({ this, &MainWindow::OnTerminalPointerWheelChanged });
        panel.SizeChanged({ this, &MainWindow::OnTerminalSizeChanged });

        // Focus the panel
        panel.Focus(FocusState::Programmatic);

        // Notify initial size
        auto size = panel.ActualSize();
        if (size.x > 0 && size.y > 0) {
            dx_notify_resize((uint32_t)size.x, (uint32_t)size.y);
            ghostty_surface_set_size(m_surface, (uint32_t)size.x, (uint32_t)size.y);
        }
    }

    ghostty_input_mods_e MainWindow::GetMods()
    {
        auto window = winrt::Microsoft::UI::Xaml::Window::Current();
        ghostty_input_mods_e mods = GHOSTTY_MODS_NONE;
        if (GetKeyState(VK_SHIFT) & 0x8000) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_SHIFT);
        if (GetKeyState(VK_CONTROL) & 0x8000) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_CTRL);
        if (GetKeyState(VK_MENU) & 0x8000) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_ALT);
        return mods;
    }

    // CharacterReceived replaced by TSF input + ToUnicode in KeyDown

    void MainWindow::OnTerminalKeyDown(
        winrt::Windows::Foundation::IInspectable const&,
        KeyRoutedEventArgs const& e)
    {
        if (!m_surface) return;

        auto key = e.Key();
        int vk = (int)key;
        auto mods = GetMods();
        bool ctrl = (mods & GHOSTTY_MODS_CTRL) != 0;

        // Ctrl+V = paste
        if (ctrl && vk == 'V') {
            if (OpenClipboard(GetWindowHandle())) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* wText = static_cast<wchar_t*>(GlobalLock(hData));
                    if (wText) {
                        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wText, -1, nullptr, 0, nullptr, nullptr);
                        if (utf8Len > 0) {
                            std::vector<char> utf8(utf8Len);
                            WideCharToMultiByte(CP_UTF8, 0, wText, -1, utf8.data(), utf8Len, nullptr, nullptr);
                            ghostty_surface_text(m_surface, utf8.data(), utf8Len - 1);
                        }
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
            e.Handled(true);
            return;
        }

        // Ctrl+C = copy selection or send key
        if (ctrl && vk == 'C') {
            if (ghostty_surface_has_selection(m_surface)) {
                ghostty_text_s text = {};
                if (ghostty_surface_read_selection(m_surface, &text) && text.text && text.text_len > 0) {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.text, (int)text.text_len, nullptr, 0);
                    if (wlen > 0 && OpenClipboard(GetWindowHandle())) {
                        EmptyClipboard();
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(wchar_t));
                        if (hMem) {
                            wchar_t* wBuf = static_cast<wchar_t*>(GlobalLock(hMem));
                            MultiByteToWideChar(CP_UTF8, 0, text.text, (int)text.text_len, wBuf, wlen);
                            wBuf[wlen] = 0;
                            GlobalUnlock(hMem);
                            SetClipboardData(CF_UNICODETEXT, hMem);
                        }
                        CloseClipboard();
                    }
                    ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, GHOSTTY_MODS_NONE);
                    ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, GHOSTTY_MODS_NONE);
                    e.Handled(true);
                    return;
                }
            }
        }

        // Try to generate text from key via ToUnicode (handles keyboard layout)
        if (!ctrl) {
            BYTE keyState[256] = {};
            GetKeyboardState(keyState);
            wchar_t chars[4] = {};
            uint32_t scancode = e.KeyStatus().ScanCode;
            int result = ToUnicode(vk, scancode, keyState, chars, 4, 0);
            if (result > 0 && chars[0] >= 0x20) {
                char utf8[8] = {};
                int len = WideCharToMultiByte(CP_UTF8, 0, chars, result, utf8, sizeof(utf8), nullptr, nullptr);
                if (len > 0) {
                    ghostty_surface_text(m_surface, utf8, len);
                    if (m_ghosttyApp.app()) ghostty_app_tick(m_ghosttyApp.app());
                    ghostty_surface_refresh(m_surface);
                    e.Handled(true);
                    return;
                }
            }
        }

        // Special keys and Ctrl+key combos → send as key event
        uint32_t scancode = e.KeyStatus().ScanCode;
        if (e.KeyStatus().IsExtendedKey) scancode |= 0xE000;

        ghostty_input_key_s keyEvent = {};
        keyEvent.action = GHOSTTY_ACTION_PRESS;
        keyEvent.keycode = scancode;
        keyEvent.mods = mods;
        keyEvent.consumed_mods = GHOSTTY_MODS_NONE;
        keyEvent.text = nullptr;
        keyEvent.unshifted_codepoint = 0;
        keyEvent.composing = false;

        ghostty_surface_key(m_surface, keyEvent);
        e.Handled(true);
    }

    void MainWindow::OnTerminalPointerMoved(
        winrt::Windows::Foundation::IInspectable const&,
        PointerRoutedEventArgs const& e)
    {
        if (!m_surface) return;
        auto pos = e.GetCurrentPoint(TerminalPanel()).Position();
        ghostty_surface_mouse_pos(m_surface, (double)pos.X, (double)pos.Y, GetMods());
    }

    void MainWindow::OnTerminalPointerPressed(
        winrt::Windows::Foundation::IInspectable const&,
        PointerRoutedEventArgs const& e)
    {
        if (!m_surface) return;
        auto props = e.GetCurrentPoint(TerminalPanel()).Properties();
        TerminalPanel().Focus(FocusState::Pointer);

        if (props.IsLeftButtonPressed())
            ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, GetMods());
        else if (props.IsRightButtonPressed()) {
            // Right click: copy selection or send right button
            if (ghostty_surface_has_selection(m_surface)) {
                ghostty_text_s text = {};
                if (ghostty_surface_read_selection(m_surface, &text) && text.text && text.text_len > 0) {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.text, (int)text.text_len, nullptr, 0);
                    if (wlen > 0 && OpenClipboard(GetWindowHandle())) {
                        EmptyClipboard();
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(wchar_t));
                        if (hMem) {
                            wchar_t* wBuf = static_cast<wchar_t*>(GlobalLock(hMem));
                            MultiByteToWideChar(CP_UTF8, 0, text.text, (int)text.text_len, wBuf, wlen);
                            wBuf[wlen] = 0;
                            GlobalUnlock(hMem);
                            SetClipboardData(CF_UNICODETEXT, hMem);
                        }
                        CloseClipboard();
                    }
                    ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, GHOSTTY_MODS_NONE);
                    ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, GHOSTTY_MODS_NONE);
                } else {
                    ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_RIGHT, GetMods());
                }
            } else {
                ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_RIGHT, GetMods());
            }
        }
        else if (props.IsMiddleButtonPressed())
            ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_MIDDLE, GetMods());

        TerminalPanel().CapturePointer(e.Pointer());
    }

    void MainWindow::OnTerminalPointerReleased(
        winrt::Windows::Foundation::IInspectable const&,
        PointerRoutedEventArgs const& e)
    {
        if (!m_surface) return;
        auto pointer = e.Pointer();

        // WinUI doesn't tell us which button was released directly in PointerReleased,
        // so release all that are no longer pressed
        auto props = e.GetCurrentPoint(TerminalPanel()).Properties();
        if (!props.IsLeftButtonPressed())
            ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, GetMods());
        if (!props.IsRightButtonPressed())
            ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_RIGHT, GetMods());
        if (!props.IsMiddleButtonPressed())
            ghostty_surface_mouse_button(m_surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_MIDDLE, GetMods());

        TerminalPanel().ReleasePointerCapture(pointer);
    }

    void MainWindow::OnTerminalPointerWheelChanged(
        winrt::Windows::Foundation::IInspectable const&,
        PointerRoutedEventArgs const& e)
    {
        if (!m_surface) return;
        auto props = e.GetCurrentPoint(TerminalPanel()).Properties();
        double delta = (double)props.MouseWheelDelta() / WHEEL_DELTA;
        ghostty_surface_mouse_scroll(m_surface, 0, delta, GetMods());
    }

    void MainWindow::OnTerminalSizeChanged(
        winrt::Windows::Foundation::IInspectable const&,
        SizeChangedEventArgs const& e)
    {
        if (!m_surface) return;
        auto size = e.NewSize();
        uint32_t w = (uint32_t)size.Width;
        uint32_t h = (uint32_t)size.Height;
        if (w > 0 && h > 0) {
            dx_notify_resize(w, h);
            ghostty_surface_set_size(m_surface, w, h);
            ghostty_surface_refresh(m_surface);
        }
    }

    // CharacterReceived / TextBox IME removed — using TSF + ToUnicode

    bool MainWindow::HandleAction(ghostty_target_s /*target*/, ghostty_action_s action)
    {
        switch (action.tag) {
        case GHOSTTY_ACTION_SET_TITLE:
            if (action.action.set_title.title) {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, action.action.set_title.title, -1, nullptr, 0);
                if (wlen > 0) {
                    std::vector<wchar_t> wTitle(wlen);
                    MultiByteToWideChar(CP_UTF8, 0, action.action.set_title.title, -1, wTitle.data(), wlen);
                    this->Title(winrt::hstring(wTitle.data()));
                }
            }
            return true;

        case GHOSTTY_ACTION_MOUSE_SHAPE: {
            // TODO: WinUI cursor changes via CoreWindow or ProtectedCursor
            return true;
        }

        case GHOSTTY_ACTION_OPEN_URL:
            if (action.action.open_url.url && action.action.open_url.len > 0) {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, action.action.open_url.url,
                    (int)action.action.open_url.len, nullptr, 0);
                if (wlen > 0) {
                    std::vector<wchar_t> wUrl(wlen + 1);
                    MultiByteToWideChar(CP_UTF8, 0, action.action.open_url.url,
                        (int)action.action.open_url.len, wUrl.data(), wlen);
                    wUrl[wlen] = 0;
                    ShellExecuteW(nullptr, L"open", wUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            return true;

        case GHOSTTY_ACTION_RING_BELL:
            MessageBeep(MB_OK);
            return true;

        case GHOSTTY_ACTION_QUIT:
            this->Close();
            return true;

        case GHOSTTY_ACTION_COLOR_CHANGE: {
            auto& cc = action.action.color_change;
            if (cc.kind == GHOSTTY_ACTION_COLOR_KIND_BACKGROUND) {
                HWND mainHwnd = GetWindowHandle();
                if (mainHwnd) {
                    COLORREF color = RGB(cc.r, cc.g, cc.b);
                    DwmSetWindowAttribute(mainHwnd, DWMWA_CAPTION_COLOR, &color, sizeof(color));
                    float luminance = 0.299f * cc.r + 0.587f * cc.g + 0.114f * cc.b;
                    COLORREF textColor = (luminance < 128) ? RGB(255, 255, 255) : RGB(0, 0, 0);
                    DwmSetWindowAttribute(mainHwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));
                }
            }
            return true;
        }

        default:
            return false;
        }
    }
}
