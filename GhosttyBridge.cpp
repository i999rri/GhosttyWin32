#include "framework.h"
#include <cstdio>
#include "GhosttyBridge.h"

#ifdef _DEBUG
#define DBG_LOG(msg) DBG_LOG(msg)
#else
#define DBG_LOG(msg) ((void)0)
#endif
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")

bool GhosttyBridge::initialize() {
    if (m_initialized) return true;

    // ghostty_init on 4MB stack thread (max_path_bytes stack overflow workaround)
    char arg0[] = "ghostty";
    char* argv[] = { arg0 };

    struct InitArgs { int argc; char** argv; int result; };
    InitArgs initArgs = { 1, argv, -1 };

    HANDLE hThread = CreateThread(
        nullptr, 4 * 1024 * 1024,
        [](LPVOID param) -> DWORD {
            auto* args = static_cast<InitArgs*>(param);
            args->result = ghostty_init(args->argc, args->argv);
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

    // Config
    m_config = ghostty_config_new();
    if (!m_config) {
        DBG_LOG("ghostty: config creation failed\n");
        return false;
    }
    ghostty_config_finalize(m_config);
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
    if (m_app) {
        ghostty_app_free(m_app);
        m_app = nullptr;
    }
    shutdownOpenGL();
    m_config = nullptr;
    m_initialized = false;
}

bool GhosttyBridge::initOpenGL(HWND hwnd) {
    m_hdc = GetDC(hwnd);
    if (!m_hdc) {
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

    int pixelFormat = ChoosePixelFormat(m_hdc, &pfd);
    if (pixelFormat == 0) {
        DBG_LOG("ghostty: ChoosePixelFormat failed\n");
        return false;
    }

    if (!SetPixelFormat(m_hdc, pixelFormat, &pfd)) {
        DBG_LOG("ghostty: SetPixelFormat failed\n");
        return false;
    }

    m_hglrc = wglCreateContext(m_hdc);
    if (!m_hglrc) {
        DBG_LOG("ghostty: wglCreateContext failed\n");
        return false;
    }

    if (!wglMakeCurrent(m_hdc, m_hglrc)) {
        DBG_LOG("ghostty: wglMakeCurrent failed\n");
        return false;
    }

    DBG_LOG("ghostty: OpenGL context created!\n");

    // Set V-Sync (0 = off, 1 = on)
    typedef BOOL (WINAPI *PFNWGLSWAPINTERVALEXTPROC)(int interval);
    auto wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    if (wglSwapIntervalEXT) {
        wglSwapIntervalEXT(m_vsync ? 1 : 0);
        DBG_LOG(m_vsync ? "ghostty: V-Sync ON\n" : "ghostty: V-Sync OFF\n");
    }

    const char* version = (const char*)glGetString(GL_VERSION);
    if (version) {
        char buf[128];
        sprintf_s(buf, "ghostty: OpenGL version: %s\n", version);
        DBG_LOG(buf);
    }

    return true;
}

void GhosttyBridge::shutdownOpenGL() {
    if (m_hglrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(m_hglrc);
        m_hglrc = nullptr;
    }
    // HDC is released when window is destroyed
    m_hdc = nullptr;
}

LRESULT CALLBACK GhosttyBridge::glWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& bridge = GhosttyBridge::instance();

    switch (msg) {
    case WM_CHAR: {
        // Convert UTF-16 wParam to UTF-8 and send to ghostty
        if (bridge.m_surface && wParam >= 0x20) {
            wchar_t utf16[2] = { (wchar_t)wParam, 0 };
            char utf8[8] = {};
            int len = WideCharToMultiByte(CP_UTF8, 0, utf16, 1, utf8, sizeof(utf8), nullptr, nullptr);
            if (len > 0) {
                ghostty_surface_text(bridge.m_surface, utf8, len);
                ghostty_surface_refresh(bridge.m_surface);
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_KEYUP: {
        if (!bridge.m_surface) break;

        // Only handle keys that don't generate WM_CHAR
        bool isSpecialKey = false;
        switch (wParam) {
            case VK_BACK: case VK_TAB: case VK_RETURN: case VK_ESCAPE:
            case VK_DELETE: case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
            case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
            case VK_INSERT: case VK_F1: case VK_F2: case VK_F3: case VK_F4:
            case VK_F5: case VK_F6: case VK_F7: case VK_F8: case VK_F9:
            case VK_F10: case VK_F11: case VK_F12:
                isSpecialKey = true;
                break;
        }
        // Ctrl+C = copy selection or send SIGINT
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'C' && msg == WM_KEYDOWN) {
            if (bridge.m_surface && ghostty_surface_has_selection(bridge.m_surface)) {
                ghostty_text_s text = {};
                if (ghostty_surface_read_selection(bridge.m_surface, &text) && text.text && text.text_len > 0) {
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
                return 0;
            }
            // No selection - fall through to send Ctrl+C as key event
        }
        // Ctrl+V = paste from clipboard directly
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'V' && msg == WM_KEYDOWN) {
            if (bridge.m_surface && bridge.m_glWindow && OpenClipboard(bridge.m_glWindow)) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* wText = static_cast<wchar_t*>(GlobalLock(hData));
                    if (wText) {
                        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wText, -1, nullptr, 0, nullptr, nullptr);
                        if (utf8Len > 0) {
                            char* utf8 = new char[utf8Len];
                            WideCharToMultiByte(CP_UTF8, 0, wText, -1, utf8, utf8Len, nullptr, nullptr);
                            ghostty_surface_text(bridge.m_surface, utf8, utf8Len - 1); // -1 for null terminator
                            delete[] utf8;
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

        ghostty_surface_key(bridge.m_surface, keyEvent);
        ghostty_surface_refresh(bridge.m_surface);
        return 0;
    }
    case WM_CLOSE:
        // Clean up ghostty before destroying the window
        if (bridge.m_surface) {
            ghostty_surface_free(bridge.m_surface);
            bridge.m_surface = nullptr;
        }
        bridge.shutdown();
        PostQuitMessage(0);
        return 0;
    case WM_SIZE: {
        if (bridge.m_surface) {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            if (width > 0 && height > 0) {
                ghostty_surface_set_size(bridge.m_surface, width, height);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (bridge.m_surface) {
            double x = (double)LOWORD(lParam);
            double y = (double)HIWORD(lParam);
            ghostty_input_mods_e mods = GHOSTTY_MODS_NONE;
            if (wParam & MK_SHIFT) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_SHIFT);
            if (wParam & MK_CONTROL) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_CTRL);
            ghostty_surface_mouse_pos(bridge.m_surface, x, y, mods);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        if (bridge.m_surface) {
            auto state = (msg == WM_LBUTTONDOWN) ? GHOSTTY_MOUSE_PRESS : GHOSTTY_MOUSE_RELEASE;
            ghostty_input_mods_e mods = GHOSTTY_MODS_NONE;
            if (wParam & MK_SHIFT) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_SHIFT);
            if (wParam & MK_CONTROL) mods = (ghostty_input_mods_e)(mods | GHOSTTY_MODS_CTRL);
            ghostty_surface_mouse_button(bridge.m_surface, state, GHOSTTY_MOUSE_LEFT, mods);
            if (msg == WM_LBUTTONDOWN) SetCapture(hwnd);
            else ReleaseCapture();
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        if (bridge.m_surface) {
            // If there's a selection, copy to clipboard
            if (ghostty_surface_has_selection(bridge.m_surface)) {
                ghostty_text_s text = {};
                if (ghostty_surface_read_selection(bridge.m_surface, &text) && text.text && text.text_len > 0) {
                    // Convert UTF-8 to UTF-16 for clipboard
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
            } else {
                // No selection - pass right click to ghostty
                ghostty_surface_mouse_button(bridge.m_surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_RIGHT, GHOSTTY_MODS_NONE);
            }
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        if (bridge.m_surface) {
            ghostty_surface_mouse_button(bridge.m_surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_RIGHT, GHOSTTY_MODS_NONE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (bridge.m_surface) {
            double delta = (double)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            ghostty_input_scroll_mods_t mods = GHOSTTY_MODS_NONE;
            ghostty_surface_mouse_scroll(bridge.m_surface, 0, delta, mods);
        }
        return 0;
    }
    case WM_SETFOCUS:
        if (bridge.m_surface) ghostty_surface_set_focus(bridge.m_surface, true);
        return 0;
    case WM_KILLFOCUS:
        if (bridge.m_surface) ghostty_surface_set_focus(bridge.m_surface, false);
        return 0;
    case WM_USER + 1:
        if (bridge.m_app) ghostty_app_tick(bridge.m_app);
        // Measure input latency
        if (bridge.m_inputPending) {
            bridge.m_inputPending = false;
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            double ms = (double)(now.QuadPart - bridge.m_lastInputTime.QuadPart) / freq.QuadPart * 1000.0;
            static int count = 0;
            if (++count % 5 == 0) {
                wchar_t title[64];
                swprintf_s(title, L"Ghostty [%.1fms]", ms);
                SetWindowTextW(hwnd, title);
            }
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND GhosttyBridge::createGLWindow(HWND parent) {
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

    HWND hwnd;
    if (parent) {
        // Child window mode
        RECT rc;
        GetClientRect(parent, &rc);
        hwnd = CreateWindowExW(
            0, L"GhosttyGLWindow", nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            0, 0, rc.right - rc.left, rc.bottom - rc.top,
            parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    } else {
        // Standalone window mode (no WinUI parent)
        hwnd = CreateWindowExW(
            0, L"GhosttyGLWindow", L"Ghostty",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 960, 640,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }

    if (hwnd) {
        char buf[128];
        sprintf_s(buf, "ghostty: GL window created: %p\n", hwnd);
        DBG_LOG(buf);
    } else {
        DBG_LOG("ghostty: failed to create GL window\n");
    }

    return hwnd;
}

ghostty_surface_t GhosttyBridge::createSurface(HWND parentHwnd) {
    if (!m_initialized || !m_app) return nullptr;

    // Create a Win32 child window for OpenGL (WinUI overwrites GDI on main window)
    m_glWindow = createGLWindow(parentHwnd);
    if (!m_glWindow) return nullptr;

    // Run on 4MB stack thread with OpenGL context
    struct Args {
        GhosttyBridge* self;
        HWND hwnd;
        ghostty_surface_t result;
    };
    Args args = { this, m_glWindow, nullptr };

    HANDLE hThread = CreateThread(
        nullptr, 4 * 1024 * 1024,
        [](LPVOID param) -> DWORD {
            auto* a = static_cast<Args*>(param);

            // Create and activate OpenGL context on this thread
            if (!a->self->initOpenGL(a->hwnd)) {
                DBG_LOG("ghostty: OpenGL init failed\n");
                return 1;
            }

            ghostty_surface_config_s surfConfig = ghostty_surface_config_new();
            surfConfig.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
            surfConfig.platform.windows.hwnd = a->hwnd;
            surfConfig.platform.windows.hdc = a->self->m_hdc;
            surfConfig.platform.windows.hglrc = a->self->m_hglrc;
            surfConfig.scale_factor = 1.0;

            a->result = ghostty_surface_new(a->self->m_app, &surfConfig);

            // Release GL context BEFORE thread exits so renderer thread can acquire it
            wglMakeCurrent(nullptr, nullptr);
            return 0;
        },
        &args, 0, nullptr);

    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
    }

    // Note: GL context remains on worker thread after it exits
    // It will need to be made current again for rendering

    if (args.result) {
        DBG_LOG("ghostty: surface created!\n");
        m_surface = args.result;
    } else {
        DBG_LOG("ghostty: surface creation failed\n");
    }
    return args.result;
}

void GhosttyBridge::destroySurface(ghostty_surface_t surface) {
    if (surface) {
        ghostty_surface_free(surface);
        DBG_LOG("ghostty: surface destroyed\n");
    }
}

// --- Stub callbacks (TODO: implement properly) ---

void GhosttyBridge::onWakeup(void* userdata) {
    auto* self = static_cast<GhosttyBridge*>(userdata);
    if (self && self->m_app) {
        ghostty_app_tick(self->m_app);
    }
}

bool GhosttyBridge::onAction(ghostty_app_t app, ghostty_target_s target, ghostty_action_s action) {
    auto& bridge = GhosttyBridge::instance();

    switch (action.tag) {
    case GHOSTTY_ACTION_SET_TITLE:
        if (action.action.set_title.title && bridge.m_glWindow) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, action.action.set_title.title, -1, nullptr, 0);
            if (wlen > 0) {
                wchar_t* wTitle = new wchar_t[wlen];
                MultiByteToWideChar(CP_UTF8, 0, action.action.set_title.title, -1, wTitle, wlen);
                SetWindowTextW(bridge.m_glWindow, wTitle);
                delete[] wTitle;
            }
        }
        return true;
    default:
        return false;
    }
}

bool GhosttyBridge::onReadClipboard(void* userdata, ghostty_clipboard_e clipboard, void* state) {
    auto* self = static_cast<GhosttyBridge*>(userdata);
    if (!self || !self->m_surface || !self->m_glWindow) return false;

    if (!OpenClipboard(self->m_glWindow)) return false;

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
        char* utf8 = new char[utf8Len];
        WideCharToMultiByte(CP_UTF8, 0, wText, -1, utf8, utf8Len, nullptr, nullptr);
        ghostty_surface_complete_clipboard_request(self->m_surface, utf8, state, false);
        delete[] utf8;
    }

    GlobalUnlock(hData);
    CloseClipboard();
    return true;
}

void GhosttyBridge::onConfirmReadClipboard(void* userdata, const char* content, void* state, ghostty_clipboard_request_e req) {
    DBG_LOG("ghostty: confirm read clipboard\n");
}

void GhosttyBridge::onWriteClipboard(void* userdata, ghostty_clipboard_e clipboard, const ghostty_clipboard_content_s* content, size_t count, bool confirm) {
    DBG_LOG("ghostty: write clipboard\n");
}

void GhosttyBridge::onCloseSurface(void* userdata, bool process_exited) {
    DBG_LOG("ghostty: close surface\n");
}
