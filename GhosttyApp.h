#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
#include <functional>
#include "ghostty/ghostty.h"

// Ghostty application wrapper (UI-independent)
// Manages ghostty_app_t and surface lifecycle.

struct SurfaceInfo {
    ghostty_surface_t surface = nullptr;
    HWND hwnd = nullptr;
};

class GhosttyApp {
public:
    GhosttyApp() = default;
    ~GhosttyApp() { shutdown(); }

    // Initialize ghostty runtime and config
    bool initialize();
    void shutdown();

    // Surface management
    ghostty_surface_t createSurface(HWND hwnd);
    void destroySurface(ghostty_surface_t surface);

    // Accessors
    ghostty_app_t app() const { return m_app; }
    ghostty_config_t config() const { return m_config; }
    bool isInitialized() const { return m_initialized; }

    // Callbacks from ghostty — set by UI layer
    std::function<void()> onWakeupFn;
    std::function<bool(ghostty_target_s, ghostty_action_s)> onActionFn;
    std::function<void(ghostty_surface_t, bool)> onCloseSurfaceFn;

private:
    // Runtime callbacks (static, forwarded to instance via userdata)
    static void onWakeup(void* userdata);
    static bool onAction(ghostty_app_t app, ghostty_target_s target, ghostty_action_s action);
    static bool onReadClipboard(void* userdata, ghostty_clipboard_e clipboard, void* state);
    static void onConfirmReadClipboard(void* userdata, const char* content, void* state, ghostty_clipboard_request_e req);
    static void onWriteClipboard(void* userdata, ghostty_clipboard_e clipboard, const ghostty_clipboard_content_s* content, size_t count, bool confirm);
    static void onCloseSurface(void* userdata, bool process_exited);

    ghostty_app_t m_app = nullptr;
    ghostty_config_t m_config = nullptr;
    bool m_initialized = false;
};
