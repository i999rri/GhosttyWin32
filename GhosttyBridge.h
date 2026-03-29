#pragma once
#include "ghostty/ghostty.h"
#include <Windows.h>
#include <GL/gl.h>
#include <cstdint>

// Bridge between libghostty and Win32
// Equivalent to the Swift AppDelegate on macOS

class GhosttyBridge {
public:
    static GhosttyBridge& instance() {
        static GhosttyBridge s;
        return s;
    }

    bool initialize();
    void shutdown();

    ghostty_app_t app() const { return m_app; }
    bool isInitialized() const { return m_initialized; }

    // Surface creation/destruction
    // Creates a Win32 child window inside parentHwnd for OpenGL rendering
    ghostty_surface_t createSurface(HWND parentHwnd);
    void destroySurface(ghostty_surface_t surface);
    HWND glWindow() const { return m_glWindow; }

private:
    GhosttyBridge() = default;
    ~GhosttyBridge() { shutdown(); }
    GhosttyBridge(const GhosttyBridge&) = delete;
    GhosttyBridge& operator=(const GhosttyBridge&) = delete;

    // Runtime callbacks (libghostty -> Win32)
    static void onWakeup(void* userdata);
    static bool onAction(ghostty_app_t app, ghostty_target_s target, ghostty_action_s action);
    static bool onReadClipboard(void* userdata, ghostty_clipboard_e clipboard, void* state);
    static void onConfirmReadClipboard(void* userdata, const char* content, void* state, ghostty_clipboard_request_e req);
    static void onWriteClipboard(void* userdata, ghostty_clipboard_e clipboard, const ghostty_clipboard_content_s* content, size_t count, bool confirm);
    static void onCloseSurface(void* userdata, bool process_exited);

    // Win32 child window for OpenGL rendering
    static LRESULT CALLBACK glWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    HWND createGLWindow(HWND parent);

    // OpenGL context for WGL
    bool initOpenGL(HWND hwnd);
    void shutdownOpenGL();

    ghostty_app_t m_app = nullptr;
    ghostty_config_t m_config = nullptr;
    ghostty_surface_t m_surface = nullptr;
    HWND m_glWindow = nullptr;
    HDC m_hdc = nullptr;
    HGLRC m_hglrc = nullptr;
    bool m_vsync = false;
    bool m_initialized = false;

    // Window state for fullscreen/decorations toggle
    bool m_fullscreen = false;
    bool m_decorations = true;
    RECT m_savedRect = {};        // Window rect before fullscreen
    DWORD m_savedStyle = 0;       // Window style before fullscreen
    uint32_t m_minWidth = 0, m_minHeight = 0;
    uint32_t m_maxWidth = 0, m_maxHeight = 0;

};
