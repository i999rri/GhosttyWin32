#include "framework.h"
#include <dwmapi.h>
#include "GhosttyBridge.h"

int APIENTRY wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    // Enable Per-Monitor DPI awareness
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Initialize libghostty
    auto& bridge = GhosttyBridge::instance();
    if (!bridge.initialize()) {
        MessageBoxW(nullptr, L"Failed to initialize Ghostty", L"Error", MB_OK);
        return 1;
    }

    // Create surface (standalone Win32 window)
    bridge.createSurface(nullptr);

    // Apply config-driven window settings
    if (HWND hwnd = bridge.glWindow()) {
        // window-decoration
        const char* decorVal = nullptr;
        if (ghostty_config_get(bridge.config(), &decorVal, "window-decoration", 17) && decorVal) {
            if (strcmp(decorVal, "false") == 0 || strcmp(decorVal, "none") == 0) {
                DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
                // Keep WS_THICKFRAME for smooth DWM resize, hide caption visually
                style = (style & ~(WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_THICKFRAME;
                bridge.setDecorations(false);
                SetWindowLongW(hwnd, GWL_STYLE, style);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            }
        }

        // background-opacity is handled by ghostty's renderer internally.
        // No Win32-level transparency needed (DWM compositing is expensive).
    }

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
