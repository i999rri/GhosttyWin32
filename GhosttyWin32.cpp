#include "framework.h"
#include <dwmapi.h>
#include <windowsx.h>
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

// Transparent drag bar overlay — sits ON TOP of the XAML Island in z-order.
// Intercepts WM_NCHITTEST: if the mouse is over interactive XAML content
// (tabs, buttons) → HTTRANSPARENT (pass to XAML below). If over empty
// space → HTCAPTION (window drag). This is the Windows Terminal approach.
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
        DBG_LOG("ghostty: XAML + WinUI 2 initialized\n");
    } catch (winrt::hresult_error const& e) {
        char buf[128];
        sprintf_s(buf, "ghostty: XAML init failed hr=0x%08X\n",
            static_cast<unsigned int>(e.code()));
        DBG_LOG(buf);
    }
    // Default to Mesa Zink (GL→Vulkan) for flicker-free OpenGL rendering.
    // Only set when using the OpenGL renderer; DirectX doesn't use Mesa.
    if (!shouldUseDirectX() && !GetEnvironmentVariableA("GALLIUM_DRIVER", nullptr, 0)) {
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
                    // Forward resize edge hits to the parent main window
                    // so the user can grab the resize border over the XAML area.
                    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
                        if (m == WM_NCHITTEST) {
                            HWND parent = GetParent(h);
                            if (parent) {
                                POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                                RECT parentRc;
                                GetWindowRect(parent, &parentRc);
                                const int border = 6;
                                if (pt.x - parentRc.left < border || parentRc.right - pt.x < border ||
                                    pt.y - parentRc.top < border || parentRc.bottom - pt.y < border) {
                                    return HTTRANSPARENT;
                                }
                            }
                        }
                        return DefWindowProcW(h, m, w, l);
                    };
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
                bridge.m_xamlHostWnd = xamlHostWnd;

                xamlSource = DesktopWindowXamlSource();
                auto interop = xamlSource.as<IDesktopWindowXamlSourceNative>();
                winrt::check_hresult(interop->AttachToWindow(xamlHostWnd));

                HWND islandHwnd = nullptr;
                winrt::check_hresult(interop->get_WindowHandle(&islandHwnd));
                session->xamlIslandHwnd = islandHwnd;
                bridge.m_xamlIslandHwnd = islandHwnd;

                // Fill the host window.
                SetWindowPos(islandHwnd, nullptr, 0, 0,
                    rc.right - rc.left, session->headerHeight,
                    SWP_SHOWWINDOW);

                // WinUI 2 TabView — activated via SxS manifest entries
                namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
                namespace xaml = winrt::Windows::UI::Xaml;

                namespace controls = xaml::Controls;
                namespace media = xaml::Media;

                // Root layout: TabView on the left, system buttons on the right
                auto root = controls::Grid();
                root.RequestedTheme(xaml::ElementTheme::Dark);
                root.Background(media::SolidColorBrush(
                    winrt::Windows::UI::ColorHelper::FromArgb(255,
                        GetRValue(bridge.m_bgColor),
                        GetGValue(bridge.m_bgColor),
                        GetBValue(bridge.m_bgColor))));
                root.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
                root.VerticalAlignment(xaml::VerticalAlignment::Stretch);

                // Three columns: TabView (auto, max 70%) + drag area (star) + buttons (auto)
                auto col1 = controls::ColumnDefinition();
                col1.Width(xaml::GridLengthHelper::Auto());
                root.ColumnDefinitions().Append(col1);
                auto col2 = controls::ColumnDefinition();
                col2.Width(xaml::GridLengthHelper::FromValueAndType(1, xaml::GridUnitType::Star));
                col2.MinWidth(100); // Always keep at least 100px for dragging
                root.ColumnDefinitions().Append(col2);
                auto col3 = controls::ColumnDefinition();
                col3.Width(xaml::GridLengthHelper::Auto());
                root.ColumnDefinitions().Append(col3);

                // TabView — capped at 70% of window width so drag area is always reachable
                auto tabView = muxc::TabView();
                tabView.HorizontalAlignment(xaml::HorizontalAlignment::Left);
                tabView.VerticalAlignment(xaml::VerticalAlignment::Stretch);
                tabView.IsAddTabButtonVisible(true);
                tabView.TabWidthMode(muxc::TabViewWidthMode::Equal);
                tabView.MaxWidth(static_cast<double>(rc.right - rc.left) * 0.7);
                controls::Grid::SetColumn(tabView, 0);

                // Update MaxWidth when window resizes
                root.SizeChanged([tabView](auto&&, xaml::SizeChangedEventArgs const& args) {
                    tabView.MaxWidth(args.NewSize().Width * 0.7);
                });

                auto tab1 = muxc::TabViewItem();
                tab1.Header(winrt::box_value(L"Terminal"));
                tab1.IsClosable(true);
                tab1.Tag(winrt::box_value(reinterpret_cast<uint64_t>(session->hwnd)));
                tabView.TabItems().Append(tab1);
                tabView.SelectedItem(tab1);
                root.Children().Append(tabView);

                HWND mainWnd = session->parentHwnd;

                // Drag area — fills the gap between tabs and system buttons.
                // Has a background so it's hittable, PointerPressed starts drag.
                auto dragArea = controls::Border();
                dragArea.Background(media::SolidColorBrush(
                    winrt::Windows::UI::ColorHelper::FromArgb(1, 30, 30, 30))); // nearly transparent but hittable
                dragArea.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
                dragArea.VerticalAlignment(xaml::VerticalAlignment::Stretch);
                controls::Grid::SetColumn(dragArea, 1);
                // Manual window drag via XAML pointer events.
                struct DragState { POINT start; bool active = false; };
                static DragState drag;
                dragArea.PointerPressed([mainWnd](auto&& sender, xaml::Input::PointerRoutedEventArgs const& args) {
                    GetCursorPos(&drag.start);
                    drag.active = true;
                    sender.as<xaml::UIElement>().CapturePointer(args.Pointer());
                    args.Handled(true);
                });
                dragArea.PointerMoved([mainWnd](auto&&, xaml::Input::PointerRoutedEventArgs const& args) {
                    if (!drag.active) return;
                    POINT now;
                    GetCursorPos(&now);
                    RECT rc;
                    GetWindowRect(mainWnd, &rc);
                    SetWindowPos(mainWnd, nullptr,
                        rc.left + (now.x - drag.start.x),
                        rc.top + (now.y - drag.start.y),
                        0, 0, SWP_NOSIZE | SWP_NOZORDER);
                    drag.start = now;
                    args.Handled(true);
                });
                dragArea.PointerReleased([](auto&& sender, xaml::Input::PointerRoutedEventArgs const& args) {
                    drag.active = false;
                    sender.as<xaml::UIElement>().ReleasePointerCapture(args.Pointer());
                    args.Handled(true);
                });
                root.Children().Append(dragArea);

                // System buttons (─ □ ×)
                auto sysButtons = controls::StackPanel();
                sysButtons.Orientation(controls::Orientation::Horizontal);
                sysButtons.VerticalAlignment(xaml::VerticalAlignment::Top);
                controls::Grid::SetColumn(sysButtons, 2);

                auto makeBtn = [&](const wchar_t* text, double w) {
                    auto btn = controls::Button();
                    auto txt = controls::TextBlock();
                    txt.Text(text);
                    txt.FontSize(10);
                    txt.Foreground(media::SolidColorBrush(
                        winrt::Windows::UI::ColorHelper::FromArgb(255, 200, 200, 200)));
                    btn.Content(txt);
                    btn.Background(media::SolidColorBrush(
                        winrt::Windows::UI::ColorHelper::FromArgb(0, 0, 0, 0)));
                    btn.Width(w);
                    btn.Height(TerminalSession::kDefaultHeaderHeight);
                    btn.Padding(xaml::ThicknessHelper::FromUniformLength(0));
                    btn.BorderThickness(xaml::ThicknessHelper::FromUniformLength(0));
                    btn.VerticalAlignment(xaml::VerticalAlignment::Top);
                    return btn;
                };

                auto minBtn = makeBtn(L"\xE949", 46);  // ChromeMinimize
                minBtn.FontFamily(media::FontFamily(L"Segoe MDL2 Assets"));
                minBtn.Click([mainWnd](auto&&, auto&&) {
                    ShowWindow(mainWnd, SW_MINIMIZE);
                });

                auto maxBtn = makeBtn(L"\xE922", 46);  // ChromeMaximize
                maxBtn.FontFamily(media::FontFamily(L"Segoe MDL2 Assets"));
                maxBtn.Click([mainWnd](auto&&, auto&&) {
                    ShowWindow(mainWnd, IsZoomed(mainWnd) ? SW_RESTORE : SW_MAXIMIZE);
                });

                auto closeBtn = makeBtn(L"\xE8BB", 46);  // ChromeClose
                closeBtn.FontFamily(media::FontFamily(L"Segoe MDL2 Assets"));
                closeBtn.Click([mainWnd](auto&&, auto&&) {
                    PostMessageW(mainWnd, WM_CLOSE, 0, 0);
                });

                sysButtons.Children().Append(minBtn);
                sysButtons.Children().Append(maxBtn);
                sysButtons.Children().Append(closeBtn);
                root.Children().Append(sysButtons);

                xamlSource.Content(root);

                // --- Tab event handlers ---
                HWND parentHwnd = mainWnd;

                // [+] button: create a new terminal session + tab
                tabView.AddTabButtonClick(
                    [parentHwnd, &bridge](muxc::TabView const& tv, auto&&) {
                        auto* newSess = bridge.createSurface(parentHwnd);
                        if (!newSess) return;
                        // Add tab and select — SelectionChanged handles
                        // DirectComposition visibility and focus.
                        auto newTab = muxc::TabViewItem();
                        newTab.Header(winrt::box_value(L"Terminal"));
                        newTab.Tag(winrt::box_value(reinterpret_cast<uint64_t>(newSess->hwnd)));
                        tv.TabItems().Append(newTab);
                        tv.SelectedItem(newTab);
                    });

                // Tab selection changed: show/hide via DirectComposition + set focus
                tabView.SelectionChanged(
                    [parentHwnd, &bridge](auto&& sender, auto&&) {
                        auto tv = sender.template as<muxc::TabView>();
                        auto sel = tv.SelectedItem();
                        if (!sel) return;
                        auto selItem = sel.template as<muxc::TabViewItem>();
                        uint64_t selTag = winrt::unbox_value<uint64_t>(selItem.Tag());
                        HWND selHwnd = reinterpret_cast<HWND>(selTag);
                        // Show/hide via DirectComposition (safe while renderers are active)
                        for (auto& s : bridge.sessions()) {
                            if (s->hwnd && s->surface) {
                                ghostty_surface_set_occlusion(s->surface, s->hwnd == selHwnd);
                            }
                        }
                        SetWindowPos(selHwnd, HWND_TOP, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE);
                        // Defer focus — XAML may steal it during event dispatch
                        PostMessageW(parentHwnd, WM_APP, 0, 0);
                    });

                // Tab close requested: destroy the session
                tabView.TabCloseRequested(
                    [parentHwnd, &bridge](muxc::TabView const& tv,
                                          muxc::TabViewTabCloseRequestedEventArgs const& args) {
                        auto item = args.Tab();
                        uint64_t tag = winrt::unbox_value<uint64_t>(item.Tag());
                        HWND targetHwnd = reinterpret_cast<HWND>(tag);

                        // Remove the tab from TabView first
                        uint32_t idx = 0;
                        if (tv.TabItems().IndexOf(item, idx)) {
                            tv.TabItems().RemoveAt(idx);
                        }

                        // If no tabs left, quit
                        if (tv.TabItems().Size() == 0) {
                            bridge.destroySession(
                                [&]() -> TerminalSession* {
                                    for (auto& s : bridge.sessions())
                                        if (s->hwnd == targetHwnd) return s.get();
                                    return nullptr;
                                }());
                            bridge.shutdown();
                            PostQuitMessage(0);
                            return;
                        }

                        // Switch to next tab FIRST via DirectComposition
                        if (auto sel = tv.SelectedItem()) {
                            auto selItem = sel.as<muxc::TabViewItem>();
                            uint64_t selTag = winrt::unbox_value<uint64_t>(selItem.Tag());
                            HWND selHwnd = reinterpret_cast<HWND>(selTag);
                            for (auto& s : bridge.sessions()) {
                                if (s->hwnd && s->surface) {
                                    ghostty_surface_set_occlusion(s->surface, s->hwnd == selHwnd);
                                }
                            }
                            SetWindowPos(selHwnd, HWND_TOP, 0, 0, 0, 0,
                                SWP_NOMOVE | SWP_NOSIZE);
                            SetFocus(selHwnd);
                        }

                        // NOW destroy the old session (new tab is already visible)
                        for (auto& s : bridge.sessions()) {
                            if (s->hwnd == targetHwnd) {
                                bridge.destroySession(s.get());
                                break;
                            }
                        }
                    });

                // When the XAML Island gets focus (user clicked the tab bar),
                // asynchronously return focus to the terminal.
                xamlSource.GotFocus(
                    [parentHwnd](DesktopWindowXamlSource const&,
                                 winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSourceGotFocusEventArgs const&) {
                        PostMessageW(parentHwnd, WM_APP, 0, 0);
                    });

                // Title sync: when ghostty sets a surface title, update the
                // matching TabViewItem header.
                static winrt::Microsoft::UI::Xaml::Controls::TabView s_tabView{ nullptr };
                static winrt::Windows::UI::Xaml::Controls::Grid s_rootGrid{ nullptr };
                s_tabView = tabView;
                s_rootGrid = root;

                // Update tab bar background when terminal bg color changes
                GhosttyBridge::s_bgColorChangedFn = [](void*, uint8_t r, uint8_t g, uint8_t b) {
                    if (!s_rootGrid) return;
                    namespace media = winrt::Windows::UI::Xaml::Media;
                    s_rootGrid.Background(media::SolidColorBrush(
                        winrt::Windows::UI::ColorHelper::FromArgb(255, r, g, b)));
                };
                GhosttyBridge::s_titleChangedFn = [](void*, HWND sessHwnd, const wchar_t* title) {
                    if (!s_tabView) return;
                    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
                    for (uint32_t i = 0; i < s_tabView.TabItems().Size(); i++) {
                        auto item = s_tabView.TabItems().GetAt(i).as<muxc::TabViewItem>();
                        uint64_t tag = winrt::unbox_value<uint64_t>(item.Tag());
                        if (reinterpret_cast<HWND>(tag) == sessHwnd) {
                            item.Header(winrt::box_value(title));
                            break;
                        }
                    }
                };

                DBG_LOG("ghostty: XAML Island attached\n");
            } catch (winrt::hresult_error const& e) {
                (void)e;
                DBG_LOG("ghostty: XAML Island failed\n");
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
