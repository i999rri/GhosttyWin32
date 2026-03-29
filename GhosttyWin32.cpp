#include "framework.h"
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

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
