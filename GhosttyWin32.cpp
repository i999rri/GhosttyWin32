#include "framework.h"
#include <dwmapi.h>
#include "GhosttyBridge.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml::Hosting;

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

    // Initialize C++/WinRT and the XAML hosting framework. The XamlApplication
    // base class from Microsoft.Toolkit.Win32.UI.XamlApplication provides the
    // metadata providers WinUI 2 controls need. Failing to initialize is not
    // fatal — the app falls back to a plain Win32 host.
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    WindowsXamlManager xamlManager{ nullptr };
    winrt::Microsoft::Toolkit::Win32::UI::XamlHost::XamlApplication xamlApp{ nullptr };
    try {
        // Empty metadata providers list — WinUI 2 controls register themselves.
        auto providers = winrt::single_threaded_vector<winrt::Windows::UI::Xaml::Markup::IXamlMetadataProvider>();
        xamlApp = winrt::Microsoft::Toolkit::Win32::UI::XamlHost::XamlApplication{ providers };
        xamlManager = WindowsXamlManager::InitializeForCurrentThread();
        OutputDebugStringA("ghostty: XAML hosting initialized\n");
    } catch (winrt::hresult_error const& e) {
        char buf[256];
        sprintf_s(buf, "ghostty: XAML init failed hr=0x%08x\n", (unsigned)e.code());
        OutputDebugStringA(buf);
    }

    // Default to Mesa Zink (GL→Vulkan) for flicker-free rendering.
    // Set before any OpenGL calls. Users can override with GALLIUM_DRIVER env var.
    if (!GetEnvironmentVariableA("GALLIUM_DRIVER", nullptr, 0)) {
        SetEnvironmentVariableA("GALLIUM_DRIVER", "zink");
    }

    // Initialize libghostty
    auto& bridge = GhosttyBridge::instance();
    if (!bridge.initialize()) {
        MessageBoxW(nullptr, L"Failed to initialize Ghostty", L"Error", MB_OK);
        return 1;
    }

    // Create surface (standalone Win32 window)
    TerminalSession* session = bridge.createSurface(nullptr);

    // Apply config-driven window settings to the top-level main window
    if (HWND hwnd = session ? session->parentHwnd : nullptr) {
        // window-decoration
        const char* decorVal = nullptr;
        if (ghostty_config_get(bridge.config(), &decorVal, "window-decoration", 17) && decorVal) {
            if (strcmp(decorVal, "false") == 0 || strcmp(decorVal, "none") == 0) {
                DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
                // Keep WS_THICKFRAME for smooth DWM resize, hide caption visually
                style = (style & ~(WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_THICKFRAME;
                session->decorations = false;
                SetWindowLongW(hwnd, GWL_STYLE, style);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            }
        }

        // Windows 11 rounded corners
        INT cornerPref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &cornerPref, sizeof(cornerPref));

        // background-opacity is handled by ghostty's renderer internally.
        // No Win32-level transparency needed (DWM compositing is expensive).
    }

    // Move focus to the rendering child so keyboard/IME input works on first
    // activation (otherwise the user has to alt-tab away and back).
    if (session && session->hwnd) {
        SetFocus(session->hwnd);
    }

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
