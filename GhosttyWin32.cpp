#include "framework.h"
#include <dwmapi.h>
#include "GhosttyBridge.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml::Hosting;

// Custom Application subclass that provides WinUI 2 metadata.
// WindowsXamlManager needs an Application with IXamlMetadataProvider
// so it can resolve WinUI 2 types (TabView etc.) at runtime.
// The actual metadata provider (XamlControlsXamlMetaDataProvider) is
// created lazily to avoid the circular dependency: the Application
// must exist before the provider can be instantiated.
struct GhosttyApp : winrt::Windows::UI::Xaml::ApplicationT<GhosttyApp,
    winrt::Windows::UI::Xaml::Markup::IXamlMetadataProvider>
{
    winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider m_provider{ nullptr };

    winrt::Windows::UI::Xaml::Markup::IXamlType GetXamlType(
        winrt::Windows::UI::Xaml::Interop::TypeName const& type)
    {
        if (!m_provider) m_provider = winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider();
        return m_provider.GetXamlType(type);
    }

    winrt::Windows::UI::Xaml::Markup::IXamlType GetXamlType(winrt::hstring const& fullName)
    {
        if (!m_provider) m_provider = winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider();
        return m_provider.GetXamlType(fullName);
    }

    winrt::com_array<winrt::Windows::UI::Xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions()
    {
        if (!m_provider) m_provider = winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider();
        return m_provider.GetXmlnsDefinitions();
    }
};

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
    DesktopWindowXamlSource xamlSource{ nullptr }; // Must outlive the message loop
    bool xamlReady = false;
    try {
        // 1. Custom App with IXamlMetadataProvider
        auto app = winrt::make<GhosttyApp>();
        // 2. Initialize XAML framework (reuses our App)
        xamlManager = WindowsXamlManager::InitializeForCurrentThread();
        // 3. Merge WinUI 2 theme resources (Fluent styles for TabView etc.)
        auto muxResources = winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources();
        winrt::Windows::UI::Xaml::Application::Current().Resources().MergedDictionaries().Append(muxResources);
        xamlReady = true;
        OutputDebugStringA("ghostty: XAML + WinUI 2 initialized\n");
    } catch (winrt::hresult_error const& e) {
        char buf[256];
        sprintf_s(buf, "ghostty: XAML init failed at hr=0x%08x\n", (unsigned)e.code());
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

        // Host a XAML Island in the header area (will become TabView in Step 2-E).
        // The island is attached to a dedicated host child window (not the main
        // window directly) so its DirectComposition surface doesn't conflict
        // with ghostty's D3D11 swap chain on the rendering child.
        if (session->headerHeight > 0 && xamlReady) {
            try {
                // Create a thin child window solely for hosting the XAML Island.
                static bool xamlHostRegistered = false;
                if (!xamlHostRegistered) {
                    WNDCLASSW wc = {};
                    wc.lpfnWndProc = DefWindowProcW;
                    wc.hInstance = GetModuleHandleW(nullptr);
                    wc.lpszClassName = L"GhosttyXamlHost";
                    RegisterClassW(&wc);
                    xamlHostRegistered = true;
                }
                RECT rc;
                GetClientRect(hwnd, &rc);
                HWND xamlHostWnd = CreateWindowExW(
                    0, L"GhosttyXamlHost", nullptr,
                    WS_CHILD | WS_VISIBLE,
                    0, 0, rc.right - rc.left, session->headerHeight,
                    hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

                session->xamlHostWnd = xamlHostWnd;

                xamlSource = DesktopWindowXamlSource();
                auto interop = xamlSource.as<IDesktopWindowXamlSourceNative>();
                winrt::check_hresult(interop->AttachToWindow(xamlHostWnd));

                HWND islandHwnd = nullptr;
                winrt::check_hresult(interop->get_WindowHandle(&islandHwnd));
                session->xamlIslandHwnd = islandHwnd;

                // Fill the host window.
                SetWindowPos(islandHwnd, nullptr, 0, 0,
                    rc.right - rc.left, session->headerHeight,
                    SWP_SHOWWINDOW);

                // WinUI 2 TabView — activated via SxS manifest entries
                namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
                namespace xaml = winrt::Windows::UI::Xaml;

                auto tabView = muxc::TabView();
                tabView.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
                tabView.VerticalAlignment(xaml::VerticalAlignment::Stretch);
                tabView.IsAddTabButtonVisible(true);
                tabView.TabWidthMode(muxc::TabViewWidthMode::Equal);

                auto tab1 = muxc::TabViewItem();
                tab1.Header(winrt::box_value(L"Terminal"));
                tab1.IsClosable(false);
                tabView.TabItems().Append(tab1);
                tabView.SelectedItem(tab1);

                xamlSource.Content(tabView);

                OutputDebugStringA("ghostty: XAML Island attached to header\n");
            } catch (winrt::hresult_error const& e) {
                char buf[256];
                sprintf_s(buf, "ghostty: XAML Island failed hr=0x%08x\n", (unsigned)e.code());
                OutputDebugStringA(buf);
            }
        }
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
