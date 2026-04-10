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

    // Initialize C++/WinRT and WinUI 2 XAML Islands. The UWP XAML runtime
    // is built into Windows — no bootstrap, resources.pri, or package
    // identity needed (unlike WinUI 3, see issue #18).
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    WindowsXamlManager xamlManager{ nullptr };
    try {
        auto providers = winrt::single_threaded_vector<winrt::Windows::UI::Xaml::Markup::IXamlMetadataProvider>();
        winrt::Microsoft::Toolkit::Win32::UI::XamlHost::XamlApplication xamlApp{ providers };
        xamlManager = WindowsXamlManager::InitializeForCurrentThread();
        OutputDebugStringA("ghostty: WinUI 2 XAML hosting initialized\n");
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

    // Apply config-driven window settings to the top-level main window.
    // The main window is always created without a native caption — the XAML
    // tab bar in the header strip replaces it. window-decoration only controls
    // whether that header strip is shown:
    //   - true  (default) → 32px header (tab bar visible, drag region present)
    //   - false           → no header   (chromeless terminal-only look)
    if (HWND hwnd = session ? session->parentHwnd : nullptr) {
        const char* decorVal = nullptr;
        if (ghostty_config_get(bridge.config(), &decorVal, "window-decoration", 17) && decorVal) {
            if (strcmp(decorVal, "false") == 0 || strcmp(decorVal, "none") == 0) {
                session->decorations = false;
                session->headerHeight = 0;
                // Re-fire WM_SIZE so the rendering child reclaims the header area.
                RECT rc;
                GetClientRect(hwnd, &rc);
                SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED,
                    MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
            }
        }

        // Windows 11 rounded corners
        INT cornerPref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &cornerPref, sizeof(cornerPref));
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
