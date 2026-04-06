#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <windowsx.h>
#include <imm.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <commctrl.h>
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;

extern "C" void dx_notify_resize(uint32_t w, uint32_t h);

namespace winrt::GhosttyWin32::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();

        // Initialize ghostty after XAML is ready
        this->Activated([this](auto&&, auto&&) {
            static bool initialized = false;
            if (!initialized) {
                initialized = true;
                InitializeTerminal();
            }
        });

        // Track WinUI window size changes to keep popup in sync
        // (WM_MOVE is handled by the subclass in InitializeTerminal)
        this->SizeChanged([this](auto&&, Microsoft::UI::Xaml::WindowSizeChangedEventArgs const&) {
            UpdateTerminalPosition();
        });
    }

    MainWindow::~MainWindow()
    {
        HWND mainHwnd = GetWindowHandle();
        if (mainHwnd) RemoveWindowSubclass(mainHwnd, MainWndSubclassProc, 1);
        if (m_surface) {
            m_ghosttyApp.destroySurface(m_surface);
            m_surface = nullptr;
        }
        if (m_terminalHwnd) {
            DestroyWindow(m_terminalHwnd);
            m_terminalHwnd = nullptr;
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

    void MainWindow::InitializeTerminal()
    {
        if (!m_ghosttyApp.initialize()) {
            OutputDebugStringA("ghostty: initialization failed\n");
            return;
        }

        // Set callbacks
        HWND mainHwnd = GetWindowHandle();

        m_ghosttyApp.onWakeupFn = [mainHwnd]() {
            if (mainHwnd) PostMessageW(mainHwnd, WM_USER + 1, 0, 0);
        };

        m_ghosttyApp.onActionFn = [this](ghostty_target_s target, ghostty_action_s action) {
            return HandleAction(target, action);
        };

        // Create child window for terminal rendering
        m_terminalHwnd = CreateTerminalWindow(mainHwnd);
        if (!m_terminalHwnd) {
            OutputDebugStringA("ghostty: failed to create terminal window\n");
            return;
        }

        // Create ghostty surface
        m_surface = m_ghosttyApp.createSurface(m_terminalHwnd);
        if (!m_surface) {
            OutputDebugStringA("ghostty: failed to create surface\n");
            return;
        }

        // Subclass WinUI HWND to track window move/minimize
        SetWindowSubclass(mainHwnd, MainWndSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

        // Sync initial size and notify ghostty
        UpdateTerminalPosition();
        {
            RECT rc;
            GetClientRect(m_terminalHwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                dx_notify_resize(w, h);
                ghostty_surface_set_size(m_surface, w, h);
            }
        }

        // Focus the terminal
        SetFocus(m_terminalHwnd);
    }

    HWND MainWindow::CreateTerminalWindow(HWND parent)
    {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = TerminalWndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"GhosttyTerminal";
            wc.style = CS_OWNDC;
            wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
            RegisterClassW(&wc);
            registered = true;
        }

        // Calculate content area in screen coordinates
        RECT rc;
        GetClientRect(parent, &rc);
        POINT pt = { rc.left, rc.top };
        ClientToScreen(parent, &pt);

        // Create as popup (owned by parent) — avoids XAML composition z-order issue
        HWND hwnd = CreateWindowExW(
            0,
            L"GhosttyTerminal", nullptr,
            WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            pt.x, pt.y, rc.right - rc.left, rc.bottom - rc.top,
            parent, nullptr, GetModuleHandleW(nullptr), nullptr);

        if (hwnd) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        }

        return hwnd;
    }

    LRESULT CALLBACK MainWindow::TerminalWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self || !self->m_surface) return DefWindowProcW(hwnd, msg, wParam, lParam);

        auto surface = self->m_surface;
        auto app = self->m_ghosttyApp.app();

        switch (msg) {
        case WM_CHAR: {
            if (wParam < 0x20) return 0;
            static wchar_t highSurrogate = 0;
            if (IS_HIGH_SURROGATE(wParam)) {
                highSurrogate = (wchar_t)wParam;
                return 0;
            }
            wchar_t utf16[3] = {};
            int utf16Len = 1;
            if (IS_LOW_SURROGATE(wParam) && highSurrogate) {
                utf16[0] = highSurrogate;
                utf16[1] = (wchar_t)wParam;
                utf16Len = 2;
                highSurrogate = 0;
            } else {
                highSurrogate = 0;
                utf16[0] = (wchar_t)wParam;
            }
            char utf8[8] = {};
            int len = WideCharToMultiByte(CP_UTF8, 0, utf16, utf16Len, utf8, sizeof(utf8), nullptr, nullptr);
            if (len > 0) {
                ghostty_surface_text(surface, utf8, len);
                if (app) ghostty_app_tick(app);
                ghostty_surface_refresh(surface);
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_KEYUP: {
            bool isSpecialKey = false;
            switch (wParam) {
                case VK_BACK: case VK_TAB: case VK_RETURN: case VK_ESCAPE:
                case VK_DELETE: case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
                case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
                case VK_INSERT: case VK_F1: case VK_F2: case VK_F3: case VK_F4:
                case VK_F5: case VK_F6: case VK_F7: case VK_F8: case VK_F9:
                case VK_F10: case VK_F11: case VK_F12:
                case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
                case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
                case VK_MENU: case VK_LMENU: case VK_RMENU:
                    isSpecialKey = true;
                    break;
            }

            // Ctrl+C = copy
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'C' && msg == WM_KEYDOWN) {
                if (ghostty_surface_has_selection(surface)) {
                    ghostty_text_s text = {};
                    if (ghostty_surface_read_selection(surface, &text) && text.text && text.text_len > 0) {
                        // Copy to clipboard
                        int wlen = MultiByteToWideChar(CP_UTF8, 0, text.text, (int)text.text_len, nullptr, 0);
                        if (wlen > 0 && OpenClipboard(hwnd)) {
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
                    }
                    // Clear selection
                    ghostty_surface_mouse_button(surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, GHOSTTY_MODS_NONE);
                    ghostty_surface_mouse_button(surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, GHOSTTY_MODS_NONE);
                    return 0;
                }
            }

            // Ctrl+V = paste
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'V' && msg == WM_KEYDOWN) {
                if (OpenClipboard(hwnd)) {
                    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                    if (hData) {
                        wchar_t* wText = static_cast<wchar_t*>(GlobalLock(hData));
                        if (wText) {
                            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wText, -1, nullptr, 0, nullptr, nullptr);
                            if (utf8Len > 0) {
                                std::vector<char> utf8(utf8Len);
                                WideCharToMultiByte(CP_UTF8, 0, wText, -1, utf8.data(), utf8Len, nullptr, nullptr);
                                ghostty_surface_text(surface, utf8.data(), utf8Len - 1);
                            }
                            GlobalUnlock(hData);
                        }
                    }
                    CloseClipboard();
                }
                return 0;
            }

            if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam >= 'A' && wParam <= 'Z')
                isSpecialKey = true;

            if (!isSpecialKey) return 0;

            uint32_t scancode = (lParam >> 16) & 0xFF;
            bool extended = (lParam >> 24) & 0x1;
            if (extended) scancode |= 0xE000;

            ghostty_input_key_s keyEvent = {};
            keyEvent.action = (msg == WM_KEYDOWN) ? GHOSTTY_ACTION_PRESS : GHOSTTY_ACTION_RELEASE;
            keyEvent.keycode = scancode;
            keyEvent.mods = GHOSTTY_MODS_NONE;
            keyEvent.consumed_mods = GHOSTTY_MODS_NONE;
            keyEvent.text = nullptr;
            keyEvent.unshifted_codepoint = 0;
            keyEvent.composing = false;

            if (GetKeyState(VK_SHIFT) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_SHIFT);
            if (GetKeyState(VK_CONTROL) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_CTRL);
            if (GetKeyState(VK_MENU) & 0x8000) keyEvent.mods = (ghostty_input_mods_e)(keyEvent.mods | GHOSTTY_MODS_ALT);

            ghostty_surface_key(surface, keyEvent);

            if (wParam == VK_CONTROL || wParam == VK_LCONTROL || wParam == VK_RCONTROL ||
                wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT ||
                wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU) {
                POINT pt;
                if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
                    ghostty_surface_mouse_pos(surface, (double)pt.x, (double)pt.y, keyEvent.mods);
                }
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_SIZE: {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            if (width > 0 && height > 0) {
                dx_notify_resize(width, height);
                ghostty_surface_set_size(surface, width, height);
                ghostty_surface_refresh(surface);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            double x = (double)GET_X_LPARAM(lParam);
            double y = (double)GET_Y_LPARAM(lParam);
            ghostty_input_mods_e mods = GHOSTTY_MODS_NONE;
            if (wParam & MK_SHIFT) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_SHIFT);
            if (wParam & MK_CONTROL) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_CTRL);
            ghostty_surface_mouse_pos(surface, x, y, mods);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP: {
            auto state = (msg == WM_LBUTTONDOWN) ? GHOSTTY_MOUSE_PRESS : GHOSTTY_MOUSE_RELEASE;
            ghostty_input_mods_e mods = GHOSTTY_MODS_NONE;
            if (wParam & MK_SHIFT) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_SHIFT);
            if (wParam & MK_CONTROL) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_CTRL);
            ghostty_surface_mouse_button(surface, state, GHOSTTY_MOUSE_LEFT, mods);
            if (msg == WM_LBUTTONDOWN) SetCapture(hwnd);
            else ReleaseCapture();
            return 0;
        }
        case WM_RBUTTONDOWN: {
            if (ghostty_surface_has_selection(surface)) {
                ghostty_text_s text = {};
                if (ghostty_surface_read_selection(surface, &text) && text.text && text.text_len > 0) {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.text, (int)text.text_len, nullptr, 0);
                    if (wlen > 0 && OpenClipboard(hwnd)) {
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
                    ghostty_surface_mouse_button(surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, GHOSTTY_MODS_NONE);
                    ghostty_surface_mouse_button(surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, GHOSTTY_MODS_NONE);
                } else {
                    ghostty_surface_mouse_button(surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_RIGHT, GHOSTTY_MODS_NONE);
                }
            } else {
                ghostty_surface_mouse_button(surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_RIGHT, GHOSTTY_MODS_NONE);
            }
            return 0;
        }
        case WM_RBUTTONUP:
            ghostty_surface_mouse_button(surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_RIGHT, GHOSTTY_MODS_NONE);
            return 0;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: {
            auto state = (msg == WM_MBUTTONDOWN) ? GHOSTTY_MOUSE_PRESS : GHOSTTY_MOUSE_RELEASE;
            ghostty_surface_mouse_button(surface, state, GHOSTTY_MOUSE_MIDDLE, GHOSTTY_MODS_NONE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            double delta = (double)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            ghostty_input_scroll_mods_t mods = GHOSTTY_MODS_NONE;
            ghostty_surface_mouse_scroll(surface, 0, delta, mods);
            return 0;
        }
        case WM_DPICHANGED: {
            UINT dpi = HIWORD(wParam);
            double scale = (double)dpi / 96.0;
            ghostty_surface_set_content_scale(surface, scale, scale);
            RECT* suggested = (RECT*)lParam;
            SetWindowPos(hwnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_IME_STARTCOMPOSITION: {
            double x = 0, y = 0, w = 0, h = 0;
            ghostty_surface_ime_point(surface, &x, &y, &w, &h);
            HIMC hImc = ImmGetContext(hwnd);
            if (hImc) {
                COMPOSITIONFORM cf = {};
                cf.dwStyle = CFS_FORCE_POSITION;
                cf.ptCurrentPos = { (LONG)x, (LONG)(y + h) };
                ImmSetCompositionWindow(hImc, &cf);
                CANDIDATEFORM cand = {};
                cand.dwIndex = 0;
                cand.dwStyle = CFS_CANDIDATEPOS;
                cand.ptCurrentPos = { (LONG)x, (LONG)(y + h) };
                ImmSetCandidateWindow(hImc, &cand);
                ImmReleaseContext(hwnd, hImc);
            }
            break;
        }
        case WM_IME_COMPOSITION: {
            if (lParam & GCS_COMPSTR) {
                HIMC hImc = ImmGetContext(hwnd);
                if (hImc) {
                    int len = ImmGetCompositionStringW(hImc, GCS_COMPSTR, nullptr, 0);
                    if (len > 0) {
                        std::vector<wchar_t> comp(len / sizeof(wchar_t) + 1);
                        ImmGetCompositionStringW(hImc, GCS_COMPSTR, comp.data(), len);
                        comp[len / sizeof(wchar_t)] = 0;
                        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, comp.data(), -1, nullptr, 0, nullptr, nullptr);
                        if (utf8Len > 0) {
                            std::vector<char> utf8(utf8Len);
                            WideCharToMultiByte(CP_UTF8, 0, comp.data(), -1, utf8.data(), utf8Len, nullptr, nullptr);
                            ghostty_surface_preedit(surface, utf8.data(), utf8Len - 1);
                        }
                    } else {
                        ghostty_surface_preedit(surface, nullptr, 0);
                    }
                    ImmReleaseContext(hwnd, hImc);
                }
            }
            if (lParam & GCS_RESULTSTR) break;
            if (lParam & GCS_COMPSTR) return 0;
            break;
        }
        case WM_IME_ENDCOMPOSITION:
            ghostty_surface_preedit(surface, nullptr, 0);
            break;
        case WM_SETFOCUS:
            ghostty_surface_set_focus(surface, true);
            return 0;
        case WM_KILLFOCUS:
            ghostty_surface_set_focus(surface, false);
            return 0;
        case WM_USER + 1:
            if (app) ghostty_app_tick(app);
            return 0;
        case WM_CLOSE:
            if (self->m_surface) {
                self->m_ghosttyApp.destroySurface(self->m_surface);
                self->m_surface = nullptr;
            }
            self->m_ghosttyApp.shutdown();
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT CALLBACK MainWindow::MainWndSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR /*subclassId*/, DWORD_PTR refData)
    {
        auto* self = reinterpret_cast<MainWindow*>(refData);
        switch (msg) {
        case WM_MOVE:
        case WM_WINDOWPOSCHANGED:
            if (self) self->UpdateTerminalPosition();
            break;
        case WM_SIZE:
            if (self) {
                if (wParam == SIZE_MINIMIZED) {
                    if (self->m_terminalHwnd) ShowWindow(self->m_terminalHwnd, SW_HIDE);
                } else {
                    if (self->m_terminalHwnd) ShowWindow(self->m_terminalHwnd, SW_SHOWNOACTIVATE);
                    self->UpdateTerminalPosition();
                }
            }
            break;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    void MainWindow::UpdateTerminalPosition()
    {
        if (!m_terminalHwnd) return;
        HWND mainHwnd = GetWindowHandle();
        if (!mainHwnd) return;

        RECT rc;
        GetClientRect(mainHwnd, &rc);
        POINT pt = { rc.left, rc.top };
        ClientToScreen(mainHwnd, &pt);

        SetWindowPos(m_terminalHwnd, nullptr,
            pt.x, pt.y,
            rc.right - rc.left, rc.bottom - rc.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

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
            LPCWSTR cursor = IDC_IBEAM;
            switch (action.action.mouse_shape) {
                case GHOSTTY_MOUSE_SHAPE_DEFAULT:   cursor = IDC_ARROW; break;
                case GHOSTTY_MOUSE_SHAPE_TEXT:       cursor = IDC_IBEAM; break;
                case GHOSTTY_MOUSE_SHAPE_POINTER:    cursor = IDC_HAND; break;
                case GHOSTTY_MOUSE_SHAPE_CROSSHAIR:  cursor = IDC_CROSS; break;
                case GHOSTTY_MOUSE_SHAPE_MOVE:
                case GHOSTTY_MOUSE_SHAPE_ALL_SCROLL:  cursor = IDC_SIZEALL; break;
                case GHOSTTY_MOUSE_SHAPE_EW_RESIZE:
                case GHOSTTY_MOUSE_SHAPE_COL_RESIZE:  cursor = IDC_SIZEWE; break;
                case GHOSTTY_MOUSE_SHAPE_NS_RESIZE:
                case GHOSTTY_MOUSE_SHAPE_ROW_RESIZE:  cursor = IDC_SIZENS; break;
                case GHOSTTY_MOUSE_SHAPE_NESW_RESIZE: cursor = IDC_SIZENESW; break;
                case GHOSTTY_MOUSE_SHAPE_NWSE_RESIZE: cursor = IDC_SIZENWSE; break;
                case GHOSTTY_MOUSE_SHAPE_NOT_ALLOWED:
                case GHOSTTY_MOUSE_SHAPE_NO_DROP:     cursor = IDC_NO; break;
                case GHOSTTY_MOUSE_SHAPE_WAIT:        cursor = IDC_WAIT; break;
                case GHOSTTY_MOUSE_SHAPE_PROGRESS:    cursor = IDC_APPSTARTING; break;
                case GHOSTTY_MOUSE_SHAPE_HELP:
                case GHOSTTY_MOUSE_SHAPE_CONTEXT_MENU: cursor = IDC_HELP; break;
                default: cursor = IDC_IBEAM; break;
            }
            SetCursor(LoadCursorW(nullptr, cursor));
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
            if (m_terminalHwnd) FlashWindow(m_terminalHwnd, TRUE);
            MessageBeep(MB_OK);
            return true;

        case GHOSTTY_ACTION_QUIT:
            if (m_terminalHwnd) PostMessageW(m_terminalHwnd, WM_CLOSE, 0, 0);
            return true;

        case GHOSTTY_ACTION_COLOR_CHANGE: {
            auto& cc = action.action.color_change;
            if (cc.kind == GHOSTTY_ACTION_COLOR_KIND_BACKGROUND && m_terminalHwnd) {
                COLORREF color = RGB(cc.r, cc.g, cc.b);
                HWND mainHwnd = GetWindowHandle();
                if (mainHwnd) {
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
