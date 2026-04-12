#include "framework.h"
#include <cstdio>
#include <string>
#include <vector>
#include <windowsx.h>  // GET_X_LPARAM, GET_Y_LPARAM
#include <shellapi.h>
#include <imm.h>
#include <dwmapi.h>
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "dwmapi.lib")
#include "GhosttyBridge.h"

#ifdef _DEBUG
#define DBG_LOG(msg) OutputDebugStringA(msg)
#else
#define DBG_LOG(msg) ((void)0)
#endif
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")

// DirectX renderer: notify resize from main thread (exported from ghostty.dll)
extern "C" void dx_notify_resize(void* dev, uint32_t w, uint32_t h);
// Get the DxDevice pointer from a ghostty surface (returns the per-surface device)
extern "C" void* ghostty_surface_dx_device(void* surface);

GhosttyBridge::TitleChangedFn GhosttyBridge::s_titleChangedFn = nullptr;
void* GhosttyBridge::s_titleChangedCtx = nullptr;

// Helper: copy UTF-8 text to Windows clipboard
static bool copyToClipboard(HWND hwnd, const char* utf8, size_t utf8Len) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)utf8Len, nullptr, 0);
    if (wlen <= 0 || !OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(wchar_t));
    if (hMem) {
        wchar_t* wBuf = static_cast<wchar_t*>(GlobalLock(hMem));
        MultiByteToWideChar(CP_UTF8, 0, utf8, (int)utf8Len, wBuf, wlen);
        wBuf[wlen] = 0;
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    }
    CloseClipboard();
    return hMem != nullptr;
}

bool GhosttyBridge::initialize() {
    if (m_initialized) return true;

    // Run ghostty_init + config on 4MB stack thread (max_path_bytes stack overflow workaround)
    char arg0[] = "ghostty";
    char* argv[] = { arg0 };

    struct InitArgs {
        int argc; char** argv;
        int result;
        ghostty_config_t config;
    };
    InitArgs initArgs = { 1, argv, -1, nullptr };

    HANDLE hThread = CreateThread(
        nullptr, 4 * 1024 * 1024,
        [](LPVOID param) -> DWORD {
            auto* args = static_cast<InitArgs*>(param);
            args->result = ghostty_init(args->argc, args->argv);
            if (args->result != 0) return 0;

            // Config (also needs large stack for file operations)
            args->config = ghostty_config_new();
            if (!args->config) return 0;

            char configPath[MAX_PATH] = {};
            if (GetEnvironmentVariableA("LOCALAPPDATA", configPath, MAX_PATH)) {
                strcat_s(configPath, "\\ghostty\\config");
                DWORD attr = GetFileAttributesA(configPath);
                if (attr != INVALID_FILE_ATTRIBUTES) {
                    ghostty_config_load_file(args->config, configPath);
                }
            }
            ghostty_config_finalize(args->config);
            return 0;
        },
        &initArgs, 0, nullptr);

    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
    }

    if (initArgs.result != 0) {
        DBG_LOG("ghostty: init failed\n");
        return false;
    }
    DBG_LOG("ghostty: init succeeded\n");

    m_config = initArgs.config;
    if (!m_config) {
        DBG_LOG("ghostty: config creation failed\n");
        return false;
    }

    // Check config diagnostics
    uint32_t diagCount = ghostty_config_diagnostics_count(m_config);
    if (diagCount > 0) {
        char buf[128];
        sprintf_s(buf, "ghostty: config has %u diagnostics\n", diagCount);
        OutputDebugStringA(buf);
        for (uint32_t i = 0; i < diagCount; i++) {
            ghostty_diagnostic_s diag = ghostty_config_get_diagnostic(m_config, i);
            if (diag.message) {
                OutputDebugStringA("ghostty: config diag: ");
                OutputDebugStringA(diag.message);
                OutputDebugStringA("\n");
            }
        }
    }
    DBG_LOG("ghostty: config finalized\n");

    // App (runtime config with callbacks)
    ghostty_runtime_config_s rtConfig = {};
    rtConfig.userdata = this;
    rtConfig.supports_selection_clipboard = false;
    rtConfig.wakeup_cb = &GhosttyBridge::onWakeup;
    rtConfig.action_cb = &GhosttyBridge::onAction;
    rtConfig.read_clipboard_cb = &GhosttyBridge::onReadClipboard;
    rtConfig.confirm_read_clipboard_cb = &GhosttyBridge::onConfirmReadClipboard;
    rtConfig.write_clipboard_cb = &GhosttyBridge::onWriteClipboard;
    rtConfig.close_surface_cb = &GhosttyBridge::onCloseSurface;

    m_app = ghostty_app_new(&rtConfig, m_config);
    if (!m_app) {
        DBG_LOG("ghostty: app creation failed\n");
        ghostty_config_free(m_config);
        m_config = nullptr;
        return false;
    }
    DBG_LOG("ghostty: app created!\n");

    m_initialized = true;
    return true;
}

void GhosttyBridge::shutdown() {
    // Free all sessions before tearing down the app
    for (auto& sess : m_sessions) {
        if (sess->surface) {
            ghostty_surface_free(sess->surface);
            sess->surface = nullptr;
        }
        shutdownOpenGL(sess.get());
    }
    m_sessions.clear();

    if (m_app) {
        ghostty_app_free(m_app);
        m_app = nullptr;
    }
    m_config = nullptr;
    m_initialized = false;
}

bool GhosttyBridge::initOpenGL(TerminalSession* session) {
    session->hdc = GetDC(session->hwnd);
    if (!session->hdc) {
        DBG_LOG("ghostty: GetDC failed\n");
        return false;
    }

    // Pixel format for OpenGL
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;

    int pixelFormat = ChoosePixelFormat(session->hdc, &pfd);
    if (pixelFormat == 0) {
        DBG_LOG("ghostty: ChoosePixelFormat failed\n");
        return false;
    }

    if (!SetPixelFormat(session->hdc, pixelFormat, &pfd)) {
        DBG_LOG("ghostty: SetPixelFormat failed\n");
        return false;
    }

    session->hglrc = wglCreateContext(session->hdc);
    if (!session->hglrc) {
        DBG_LOG("ghostty: wglCreateContext failed\n");
        return false;
    }

    if (!wglMakeCurrent(session->hdc, session->hglrc)) {
        DBG_LOG("ghostty: wglMakeCurrent failed\n");
        return false;
    }

    DBG_LOG("ghostty: OpenGL context created!\n");

    // Set V-Sync (0 = off, 1 = on)
    typedef BOOL (WINAPI *PFNWGLSWAPINTERVALEXTPROC)(int interval);
    auto wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    if (wglSwapIntervalEXT) {
        wglSwapIntervalEXT(0); // V-Sync OFF for low latency
        DBG_LOG("ghostty: V-Sync OFF\n");
    }

    const char* version = (const char*)glGetString(GL_VERSION);
    if (version) {
        char buf[128];
        sprintf_s(buf, "ghostty: OpenGL version: %s\n", version);
        DBG_LOG(buf);
    }

    return true;
}

void GhosttyBridge::shutdownOpenGL(TerminalSession* session) {
    if (session->hglrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(session->hglrc);
        session->hglrc = nullptr;
    }
    // HDC is released when window is destroyed
    session->hdc = nullptr;
}

TerminalSession* GhosttyBridge::sessionFromSurface(ghostty_surface_t surface) {
    if (!surface) return nullptr;
    return static_cast<TerminalSession*>(ghostty_surface_userdata(surface));
}

LRESULT CALLBACK GhosttyBridge::glWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Stash the TerminalSession* passed via CreateWindowEx lpParam
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }

    auto* sess = reinterpret_cast<TerminalSession*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    auto& bridge = GhosttyBridge::instance();

    // Helper that's true only when this window has a fully-initialized session
    const bool hasSurface = sess && sess->surface;

    switch (msg) {
    case WM_CHAR: {
        if (!hasSurface || wParam < 0x20) return 0;
        // Handle UTF-16 surrogate pairs (emoji etc.)
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
            ghostty_surface_text(sess->surface, utf8, len);
            if (bridge.m_app) ghostty_app_tick(bridge.m_app);
            ghostty_surface_refresh(sess->surface);
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_KEYUP: {
        if (!hasSurface) break;

        // Only handle keys that don't generate WM_CHAR
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
        // Ctrl+C = copy selection or send SIGINT
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'C' && msg == WM_KEYDOWN) {
            if (ghostty_surface_has_selection(sess->surface)) {
                ghostty_text_s text = {};
                if (ghostty_surface_read_selection(sess->surface, &text) && text.text && text.text_len > 0) {
                    copyToClipboard(hwnd, text.text, text.text_len);
                }
                // Clear selection
                ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, GHOSTTY_MODS_NONE);
                ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, GHOSTTY_MODS_NONE);

                return 0;
            }
            // No selection - fall through to send Ctrl+C as key event
        }
        // Ctrl+V = paste from clipboard directly
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
                            ghostty_surface_text(sess->surface, utf8.data(), utf8Len - 1);
                        }
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
            return 0;
        }
        // Ctrl+letter combos (Ctrl+C, etc.)
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam >= 'A' && wParam <= 'Z')
            isSpecialKey = true;

        if (!isSpecialKey) return 0;

        // Extract scan code from lParam (bits 16-23)
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

        ghostty_surface_key(sess->surface, keyEvent);

        // When modifier keys change, re-send mouse position so ghostty
        // can update link detection (e.g. Ctrl+hover to highlight URLs)
        if (wParam == VK_CONTROL || wParam == VK_LCONTROL || wParam == VK_RCONTROL ||
            wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT ||
            wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU) {
            POINT pt;
            if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
                ghostty_surface_mouse_pos(sess->surface, (double)pt.x, (double)pt.y, keyEvent.mods);
            }
        }

        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // Skip background erase to prevent flicker
    case WM_PAINT: {
        // Validate the region without drawing - renderer handles all drawing
        ValidateRect(hwnd, nullptr);
        return 0;
    }
    case WM_SIZE: {
        UINT width = LOWORD(lParam);
        UINT height = HIWORD(lParam);
        if (width > 0 && height > 0) {
            // Notify the per-surface DirectX device of the new size
            if (hasSurface) {
                void* dxDev = ghostty_surface_dx_device(sess->surface);
                dx_notify_resize(dxDev, width, height);
            }
            if (hasSurface) {
                ghostty_surface_set_size(sess->surface, width, height);
                ghostty_surface_refresh(sess->surface);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (hasSurface) {
            double x = (double)GET_X_LPARAM(lParam);
            double y = (double)GET_Y_LPARAM(lParam);
            ghostty_input_mods_e mods = GHOSTTY_MODS_NONE;
            if (wParam & MK_SHIFT) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_SHIFT);
            if (wParam & MK_CONTROL) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_CTRL);
            ghostty_surface_mouse_pos(sess->surface, x, y, mods);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        if (hasSurface) {
            auto state = (msg == WM_LBUTTONDOWN) ? GHOSTTY_MOUSE_PRESS : GHOSTTY_MOUSE_RELEASE;
            ghostty_input_mods_e mods = GHOSTTY_MODS_NONE;
            if (wParam & MK_SHIFT) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_SHIFT);
            if (wParam & MK_CONTROL) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_CTRL);
            ghostty_surface_mouse_button(sess->surface, state, GHOSTTY_MOUSE_LEFT, mods);
            if (msg == WM_LBUTTONDOWN) SetCapture(hwnd);
            else ReleaseCapture();
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        if (hasSurface) {
            if (ghostty_surface_has_selection(sess->surface)) {
                ghostty_text_s text = {};
                if (ghostty_surface_read_selection(sess->surface, &text) && text.text && text.text_len > 0) {
                    copyToClipboard(hwnd, text.text, text.text_len);
                }
                // Clear selection
                ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, GHOSTTY_MODS_NONE);
                ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, GHOSTTY_MODS_NONE);

            } else {
                ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_RIGHT, GHOSTTY_MODS_NONE);
            }
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        if (hasSurface) {
            ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_RIGHT, GHOSTTY_MODS_NONE);
        }
        return 0;
    }
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        if (hasSurface) {
            auto state = (msg == WM_MBUTTONDOWN) ? GHOSTTY_MOUSE_PRESS : GHOSTTY_MOUSE_RELEASE;
            ghostty_surface_mouse_button(sess->surface, state, GHOSTTY_MOUSE_MIDDLE, GHOSTTY_MODS_NONE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (hasSurface) {
            double delta = (double)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            ghostty_input_scroll_mods_t mods = GHOSTTY_MODS_NONE;
            ghostty_surface_mouse_scroll(sess->surface, 0, delta, mods);
        }
        return 0;
    }
    case WM_IME_STARTCOMPOSITION: {
        // Position the IME candidate window near the cursor
        if (hasSurface) {
            double x = 0, y = 0, w = 0, h = 0;
            ghostty_surface_ime_point(sess->surface, &x, &y, &w, &h);
            HIMC hImc = ImmGetContext(hwnd);
            if (hImc) {
                // Position the composition window
                COMPOSITIONFORM cf = {};
                cf.dwStyle = CFS_FORCE_POSITION;
                cf.ptCurrentPos = { (LONG)x, (LONG)(y + h) };
                ImmSetCompositionWindow(hImc, &cf);

                // Position the candidate window below the composition
                CANDIDATEFORM cand = {};
                cand.dwIndex = 0;
                cand.dwStyle = CFS_CANDIDATEPOS;
                cand.ptCurrentPos = { (LONG)x, (LONG)(y + h) };
                ImmSetCandidateWindow(hImc, &cand);

                ImmReleaseContext(hwnd, hImc);
            }
        }
        break;
    }
    case WM_IME_COMPOSITION: {
        if (hasSurface && (lParam & GCS_COMPSTR)) {
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
                        ghostty_surface_preedit(sess->surface, utf8.data(), utf8Len - 1);
                    }
                } else {
                    // Empty composition = cleared
                    ghostty_surface_preedit(sess->surface, nullptr, 0);
                }
                ImmReleaseContext(hwnd, hImc);
            }
        }
        if (lParam & GCS_RESULTSTR) {
            // Let WM_CHAR handle the committed text
            break;
        }
        if (lParam & GCS_COMPSTR) {
            return 0; // We handled the composition
        }
        break;
    }
    case WM_IME_ENDCOMPOSITION:
        if (hasSurface) {
            ghostty_surface_preedit(sess->surface, nullptr, 0);
        }
        break;
    case WM_SETFOCUS:
        if (hasSurface) ghostty_surface_set_focus(sess->surface, true);
        return 0;
    case WM_KILLFOCUS: {
        if (hasSurface) ghostty_surface_set_focus(sess->surface, false);
        // If focus moved to the XAML Island (tab bar click), schedule a
        // focus return. The PostMessage lets the click be processed first.
        HWND newFocus = (HWND)wParam;
        if (newFocus && sess && sess->parentHwnd &&
            IsChild(sess->parentHwnd, newFocus) && newFocus != hwnd) {
            PostMessageW(sess->parentHwnd, WM_APP, 0, 0);
        }
        return 0;
    }
    case WM_USER + 1:
        // Wakeup from ghostty - process pending events on main thread
        if (bridge.m_app) ghostty_app_tick(bridge.m_app);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK GhosttyBridge::mainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }

    auto* sess = reinterpret_cast<TerminalSession*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    auto& bridge = GhosttyBridge::instance();

    switch (msg) {
    case WM_NCCALCSIZE:
        // The window has WS_THICKFRAME but no WS_CAPTION. By default Windows
        // would still leave a thin resize border in the NC area; we extend the
        // client area to cover the entire window so our XAML header reaches
        // the top edge. Resize edges are handled manually via WM_NCHITTEST.
        if (wParam == TRUE) return 0;
        break;
    case WM_NCHITTEST: {
        // Manual hit-testing: resize edges → header drag region → client.
        if (!sess) break;
        RECT rc;
        GetClientRect(hwnd, &rc);
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        const int border = 4;  // thin resize border (sides + bottom)
        bool left = pt.x < border;
        bool right = pt.x >= rc.right - border;
        bool top = pt.y < border;
        bool bottom = pt.y >= rc.bottom - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        // Header area (tab bar strip): check if a XAML element is under
        // the cursor. If not (empty space), return HTCAPTION for drag.
        if (sess->headerHeight > 0 && pt.y < sess->headerHeight) {
            // Ask the XAML Island's interop hwnd if it wants this point.
            // ChildWindowFromPoint skips the drag bar and checks the
            // XAML host; if the deepest child is the host itself (no
            // interactive element), treat it as draggable empty space.
            POINT screenPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            HWND hit = WindowFromPoint(screenPt);
            // If the hit window is the main window itself or a non-XAML
            // child, it's empty header space — allow dragging.
            if (hit == hwnd) return HTCAPTION;
            // Check if the hit window is an interactive XAML element by
            // walking up to see if it's a child of our main window.
            // The XAML Island's internal windows handle their own clicks.
            // For the gap between TabView and buttons (drag area column),
            // the XAML host window receives the hit but has no interactive
            // content → we detect this by checking the class name.
            wchar_t cls[64] = {};
            GetClassNameW(hit, cls, 64);
            // "Windows.UI.Input.InputSite.WindowClass" is the XAML
            // Island's top-level input window. If it's hit directly
            // (not a button or tab inside it), treat as drag area.
            if (wcsstr(cls, L"InputSite") || wcsstr(cls, L"XamlHost")) {
                return HTCAPTION;
            }
        }
        return HTCLIENT;
    }

    case WM_GETMINMAXINFO:
        if (sess) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            if (sess->minWidth > 0) mmi->ptMinTrackSize.x = sess->minWidth;
            if (sess->minHeight > 0) mmi->ptMinTrackSize.y = sess->minHeight;
            if (sess->maxWidth > 0) mmi->ptMaxTrackSize.x = sess->maxWidth;
            if (sess->maxHeight > 0) mmi->ptMaxTrackSize.y = sess->maxHeight;
        }
        return 0;

    case WM_SIZE: {
        if (!sess) return 0;
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        int top = sess->headerHeight;
        int childHeight = height - top;
        if (childHeight < 1) childHeight = 1;
        if (sess->xamlHostWnd && top > 0) {
            SetWindowPos(sess->xamlHostWnd, nullptr, 0, 0, width, top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (sess->xamlIslandHwnd) {
                SetWindowPos(sess->xamlIslandHwnd, nullptr, 0, 0, width, top,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        }
        // Resize the rendering child below the header.
        if (sess->hwnd) {
            SetWindowPos(sess->hwnd, nullptr, 0, top, width, childHeight,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_SETFOCUS:
        // Forward focus to the rendering child so it receives keyboard input.
        if (sess && sess->hwnd) SetFocus(sess->hwnd);
        return 0;

    case WM_ACTIVATE:
        // When the window is activated (clicked, alt-tabbed to), force focus
        // to the Ghostty child. The XAML Island's internal windows tend to
        // capture focus otherwise, preventing terminal keyboard input.
        if (LOWORD(wParam) != WA_INACTIVE && sess && sess->hwnd) {
            SetFocus(sess->hwnd);
        }
        return 0;

    case WM_DPICHANGED:
        if (sess && sess->surface) {
            UINT dpi = HIWORD(wParam);
            double scale = (double)dpi / 96.0;
            ghostty_surface_set_content_scale(sess->surface, scale, scale);
            RECT* suggested = (RECT*)lParam;
            SetWindowPos(hwnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_APP:
        // Posted by XAML Island's GotFocus / SelectionChanged — return focus
        // to whichever terminal session is currently visible.
        for (auto& s : bridge.m_sessions) {
            if (s->hwnd && IsWindowVisible(s->hwnd)) {
                SetFocus(s->hwnd);
                break;
            }
        }
        return 0;

    case WM_USER + 1:
        // Wakeup from ghostty — process pending events on main thread.
        // Posted by onWakeup() which targets m_wakeupHwnd (this window).
        if (bridge.m_app) ghostty_app_tick(bridge.m_app);
        return 0;

    case WM_CLOSE:
        // Phase 2-A: still single-session, so closing the main window quits the app.
        if (sess) bridge.destroySession(sess);
        bridge.shutdown();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND GhosttyBridge::createMainWindow(TerminalSession* session) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = mainWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GhosttyMainWindow";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
        RegisterClassW(&wc);
        registered = true;
    }

    // No native caption — the XAML tab bar in the header area replaces it.
    // Keep WS_THICKFRAME for resize, WS_SYSMENU/MIN/MAX for taskbar interaction.
    const DWORD style = WS_OVERLAPPED | WS_THICKFRAME | WS_SYSMENU
                      | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE;
    return CreateWindowExW(
        0, L"GhosttyMainWindow", L"Ghostty",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 640,
        nullptr, nullptr, GetModuleHandleW(nullptr), session);
}

HWND GhosttyBridge::createGLWindow(HWND parent, TerminalSession* session) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = glWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GhosttyGLWindow";
        wc.style = CS_OWNDC;
        wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
        RegisterClassW(&wc);
        registered = true;
    }

    RECT rc;
    GetClientRect(parent, &rc);
    int top = session ? session->headerHeight : 0;
    HWND hwnd = CreateWindowExW(
        0, L"GhosttyGLWindow", nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, top, rc.right - rc.left, (rc.bottom - rc.top) - top,
        parent, nullptr, GetModuleHandleW(nullptr), session);

    if (hwnd) {
        char buf[128];
        sprintf_s(buf, "ghostty: GL window created: %p\n", hwnd);
        DBG_LOG(buf);
    } else {
        DBG_LOG("ghostty: failed to create GL window\n");
    }

    return hwnd;
}

TerminalSession* GhosttyBridge::createSurface(HWND parentHwnd) {
    if (!m_initialized || !m_app) return nullptr;

    auto sessionOwned = std::make_unique<TerminalSession>();
    TerminalSession* session = sessionOwned.get();

    // If no parent was supplied, host the rendering child inside our own
    // top-level main window (Phase 2-A: always one main window per session).
    if (!parentHwnd) {
        session->parentHwnd = createMainWindow(session);
        if (!session->parentHwnd) return nullptr;
        // Force the NC area recalculation so our WM_NCCALCSIZE (which extends
        // the client area to cover the entire window) takes effect immediately.
        // Without this, the first frame can flash a native caption remnant.
        SetWindowPos(session->parentHwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        parentHwnd = session->parentHwnd;
    } else {
        session->parentHwnd = parentHwnd;
    }

    // Store main window HWND for thread-safe wakeup (set once, never changes)
    if (!m_wakeupHwnd) {
        m_wakeupHwnd = session->parentHwnd;
    }

    // Create the rendering child — session pointer is plumbed through WM_NCCREATE
    session->hwnd = createGLWindow(parentHwnd, session);
    if (!session->hwnd) {
        if (session->parentHwnd) DestroyWindow(session->parentHwnd);
        return nullptr;
    }

    // Run on 4MB stack thread with renderer context
    struct Args {
        GhosttyBridge* self;
        TerminalSession* session;
    };
    Args args = { this, session };

    HANDLE hThread = CreateThread(
        nullptr, 4 * 1024 * 1024,
        [](LPVOID param) -> DWORD {
            auto* a = static_cast<Args*>(param);
            TerminalSession* sess = a->session;

            // Default to DirectX, use GHOSTTY_RENDERER=opengl to override
            char rendererBuf[32] = {};
            GetEnvironmentVariableA("GHOSTTY_RENDERER", rendererBuf, sizeof(rendererBuf));
            bool useDirectX = (strcmp(rendererBuf, "opengl") != 0);
            {
                char logBuf[128];
                sprintf_s(logBuf, "ghostty: GHOSTTY_RENDERER=%s useDirectX=%d\n", rendererBuf, useDirectX);
                OutputDebugStringA(logBuf);
            }

            if (!useDirectX) {
                // Create and activate OpenGL context on this thread
                if (!initOpenGL(sess)) {
                    DBG_LOG("ghostty: OpenGL init failed\n");
                    return 1;
                }
            }

            ghostty_surface_config_s surfConfig = ghostty_surface_config_new();
            surfConfig.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
            surfConfig.platform.windows.hwnd = sess->hwnd;
            surfConfig.platform.windows.hdc = useDirectX ? nullptr : sess->hdc;
            surfConfig.platform.windows.hglrc = useDirectX ? nullptr : sess->hglrc;
            // Stored on the surface so onAction can recover the session via
            // ghostty_surface_userdata(target.surface).
            surfConfig.userdata = sess;
            // Get DPI scale factor
            UINT dpi = GetDpiForWindow(sess->hwnd);
            surfConfig.scale_factor = (double)dpi / 96.0;

            {
                char buf[256];
                sprintf_s(buf, "ghostty: calling ghostty_surface_new app=%p hwnd=%p sessions=%zu\n",
                    a->self->m_app, sess->hwnd, a->self->m_sessions.size());
                OutputDebugStringA(buf);
            }

            __try {
                sess->surface = ghostty_surface_new(a->self->m_app, &surfConfig);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                char buf[128];
                sprintf_s(buf, "ghostty: CRASH in ghostty_surface_new! code=0x%08X\n",
                    GetExceptionCode());
                OutputDebugStringA(buf);
                sess->surface = nullptr;
            }

            // Release GL context BEFORE thread exits so renderer thread can acquire it
            if (!useDirectX) wglMakeCurrent(nullptr, nullptr);
            return 0;
        },
        &args, 0, nullptr);

    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
    }

    if (!session->surface) {
        DBG_LOG("ghostty: surface creation failed\n");
        DestroyWindow(session->hwnd);
        return nullptr;
    }

    DBG_LOG("ghostty: surface created!\n");
    m_sessions.push_back(std::move(sessionOwned));
    return session;
}

void GhosttyBridge::destroySession(TerminalSession* session) {
    if (!session) return;
    if (session->surface) {
        ghostty_surface_free(session->surface);
        session->surface = nullptr;
    }
    shutdownOpenGL(session);
    // Clear userdata so pending messages on these hwnds see no session.
    if (session->hwnd) {
        SetWindowLongPtrW(session->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(session->hwnd);
        session->hwnd = nullptr;
    }
    if (session->parentHwnd) {
        SetWindowLongPtrW(session->parentHwnd, GWLP_USERDATA, 0);
        // Don't destroy parentHwnd — it's the shared main window for all tabs.
    }
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (it->get() == session) {
            m_sessions.erase(it);
            break;
        }
    }
    DBG_LOG("ghostty: session destroyed\n");
}

// --- Stub callbacks (TODO: implement properly) ---

void GhosttyBridge::onWakeup(void* userdata) {
    auto* self = static_cast<GhosttyBridge*>(userdata);
    if (!self || !self->m_wakeupHwnd) return;
    // Post to the main window — avoids accessing m_sessions which can be
    // reallocated on the main thread while renderer threads call this.
    PostMessageW(self->m_wakeupHwnd, WM_USER + 1, 0, 0);
}

bool GhosttyBridge::onAction(ghostty_app_t app, ghostty_target_s target, ghostty_action_s action) {
    auto& bridge = GhosttyBridge::instance();

    // Resolve the target session for surface-targeted actions.
    // App-targeted actions (NEW_WINDOW etc.) leave session=nullptr.
    TerminalSession* sess = nullptr;
    if (target.tag == GHOSTTY_TARGET_SURFACE) {
        sess = sessionFromSurface(target.target.surface);
    }
    // Window-level actions (title, fullscreen, decorations, color, ...) target
    // the top-level main window. The rendering child does not own a frame.
    HWND hwnd = sess ? sess->parentHwnd : nullptr;

    switch (action.tag) {
    case GHOSTTY_ACTION_SET_TITLE:
        if (action.action.set_title.title && hwnd) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, action.action.set_title.title, -1, nullptr, 0);
            if (wlen > 0) {
                std::vector<wchar_t> wTitle(wlen);
                MultiByteToWideChar(CP_UTF8, 0, action.action.set_title.title, -1, wTitle.data(), wlen);
                SetWindowTextW(hwnd, wTitle.data());
                // Update the matching TabViewItem header
                HWND sessHwnd = sess ? sess->hwnd : nullptr;
                if (sessHwnd && s_titleChangedFn) {
                    s_titleChangedFn(s_titleChangedCtx, sessHwnd, wTitle.data());
                }
            }
        }
        return true;

    case GHOSTTY_ACTION_MOUSE_SHAPE: {
        LPCWSTR cursor = IDC_IBEAM; // default for terminal
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

    case GHOSTTY_ACTION_MOUSE_VISIBILITY:
        // TODO: ShowCursor uses a counter and is tricky to balance.
        // For now, just change the cursor instead of hiding it.
        if (action.action.mouse_visibility == GHOSTTY_MOUSE_HIDDEN)
            SetCursor(nullptr);
        else
            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        return true;

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
        if (hwnd)
            FlashWindow(hwnd, TRUE);
        MessageBeep(MB_OK);
        return true;

    case GHOSTTY_ACTION_QUIT:
        if (hwnd)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return true;

    case GHOSTTY_ACTION_TOGGLE_FULLSCREEN: {
        if (!sess || !hwnd) return true;
        sess->fullscreen = !sess->fullscreen;
        if (sess->fullscreen) {
            // Save current state
            sess->savedStyle = GetWindowLongW(hwnd, GWL_STYLE);
            GetWindowRect(hwnd, &sess->savedRect);
            // Go fullscreen
            SetWindowLongW(hwnd, GWL_STYLE, sess->savedStyle & ~(WS_OVERLAPPEDWINDOW));
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowPos(hwnd, HWND_TOP,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_FRAMECHANGED);
        } else {
            // Restore
            SetWindowLongW(hwnd, GWL_STYLE, sess->savedStyle);
            SetWindowPos(hwnd, nullptr,
                sess->savedRect.left, sess->savedRect.top,
                sess->savedRect.right - sess->savedRect.left,
                sess->savedRect.bottom - sess->savedRect.top,
                SWP_FRAMECHANGED | SWP_NOZORDER);
        }
        return true;
    }

    case GHOSTTY_ACTION_TOGGLE_MAXIMIZE:
        if (hwnd) {
            if (IsZoomed(hwnd))
                ShowWindow(hwnd, SW_RESTORE);
            else
                ShowWindow(hwnd, SW_MAXIMIZE);
        }
        return true;

    case GHOSTTY_ACTION_TOGGLE_WINDOW_DECORATIONS:
        // The native caption is always absent — toggling decorations shows or
        // hides the XAML header strip (and with it, tabs and the drag region).
        if (sess && hwnd) {
            sess->decorations = !sess->decorations;
            sess->headerHeight = sess->decorations ? 32 : 0;
            RECT rc;
            GetClientRect(hwnd, &rc);
            SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED,
                MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
        }
        return true;

    case GHOSTTY_ACTION_SIZE_LIMIT: {
        if (sess) {
            sess->minWidth = action.action.size_limit.min_width;
            sess->minHeight = action.action.size_limit.min_height;
            sess->maxWidth = action.action.size_limit.max_width;
            sess->maxHeight = action.action.size_limit.max_height;
            char buf[128];
            sprintf_s(buf, "ghostty: SIZE_LIMIT min=%ux%u max=%ux%u\n",
                sess->minWidth, sess->minHeight, sess->maxWidth, sess->maxHeight);
            OutputDebugStringA(buf);
        }
        return true;
    }

    case GHOSTTY_ACTION_INITIAL_SIZE:
        if (hwnd && action.action.initial_size.width > 0 && action.action.initial_size.height > 0) {
            SetWindowPos(hwnd, nullptr, 0, 0,
                action.action.initial_size.width,
                action.action.initial_size.height,
                SWP_NOMOVE | SWP_NOZORDER);
        }
        return true;

    case GHOSTTY_ACTION_RESET_WINDOW_SIZE:
        if (hwnd) {
            SetWindowPos(hwnd, nullptr, 0, 0, 960, 640,
                SWP_NOMOVE | SWP_NOZORDER);
        }
        return true;

    case GHOSTTY_ACTION_COLOR_CHANGE: {
        auto& cc = action.action.color_change;
        if (cc.kind == GHOSTTY_ACTION_COLOR_KIND_BACKGROUND && hwnd) {
            // Set title bar color to match terminal background (Windows 10/11)
            COLORREF color = RGB(cc.r, cc.g, cc.b);
            DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &color, sizeof(color));

            // Also set title bar text color: light text on dark bg, dark text on light bg
            float luminance = 0.299f * cc.r + 0.587f * cc.g + 0.114f * cc.b;
            COLORREF textColor = (luminance < 128) ? RGB(255, 255, 255) : RGB(0, 0, 0);
            DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));
        }
        return true;
    }

    case GHOSTTY_ACTION_DESKTOP_NOTIFICATION:
        if (action.action.desktop_notification.title) {
            // Simple notification via balloon tooltip or message box
            // TODO: use proper Windows toast notifications
            MessageBeep(MB_ICONINFORMATION);
            if (hwnd) FlashWindow(hwnd, TRUE);
        }
        return true;

    default:
        return false;
    }
}

bool GhosttyBridge::onReadClipboard(void* userdata, ghostty_clipboard_e clipboard, void* state) {
    auto* self = static_cast<GhosttyBridge*>(userdata);
    // TODO(tabs): the runtime callback doesn't expose which surface requested
    // the read. For Phase 1 (single session) we always use the front session;
    // Phase 3 will need to track the focused session.
    if (!self || self->m_sessions.empty()) return false;
    TerminalSession* sess = self->m_sessions.front().get();
    if (!sess->surface || !sess->parentHwnd) return false;

    if (!OpenClipboard(sess->parentHwnd)) return false;

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        return false;
    }

    wchar_t* wText = static_cast<wchar_t*>(GlobalLock(hData));
    if (!wText) {
        CloseClipboard();
        return false;
    }

    // Convert UTF-16 to UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wText, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len > 0) {
        std::vector<char> utf8(utf8Len);
        WideCharToMultiByte(CP_UTF8, 0, wText, -1, utf8.data(), utf8Len, nullptr, nullptr);
        ghostty_surface_complete_clipboard_request(sess->surface, utf8.data(), state, false);
    }

    GlobalUnlock(hData);
    CloseClipboard();
    return true;
}

void GhosttyBridge::onConfirmReadClipboard(void* userdata, const char* content, void* state, ghostty_clipboard_request_e req) {
    DBG_LOG("ghostty: confirm read clipboard\n");
}

void GhosttyBridge::onWriteClipboard(void* userdata, ghostty_clipboard_e clipboard, const ghostty_clipboard_content_s* content, size_t count, bool confirm) {
    auto* self = static_cast<GhosttyBridge*>(userdata);
    // TODO(tabs): same caveat as onReadClipboard — Phase 1 uses the front session's hwnd.
    if (!self || self->m_sessions.empty() || count == 0 || !content) return;
    HWND hwnd = self->m_sessions.front()->parentHwnd;
    if (!hwnd) return;

    // Find text content
    const char* text = nullptr;
    for (size_t i = 0; i < count; i++) {
        if (content[i].data) {
            text = content[i].data;
            break;
        }
    }
    if (!text || !text[0]) return;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (wlen <= 0 || !OpenClipboard(hwnd)) return;

    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(wchar_t));
    if (hMem) {
        wchar_t* wBuf = static_cast<wchar_t*>(GlobalLock(hMem));
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wBuf, wlen);
        wBuf[wlen] = 0;
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    }
    CloseClipboard();
}

void GhosttyBridge::onCloseSurface(void* userdata, bool process_exited) {
    // TODO: exit does not close window yet - needs investigation
    // onCloseSurface is called but PostMessage/DestroyWindow doesn't work
    // from the callback thread. May need a different approach.
}
