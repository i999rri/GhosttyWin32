#include "framework.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <microsoft.ui.xaml.window.h>
#include "GhosttyBridge.h"

using namespace winrt;

struct GhosttyApp : winrt::Microsoft::UI::Xaml::ApplicationT<GhosttyApp>
{
    void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
        namespace xaml = winrt::Microsoft::UI::Xaml;
        namespace media = xaml::Media;

        // Pre-define Acrylic fallback brushes BEFORE loading XamlControlsResources.
        // XamlControlsResources internally references these system brushes which
        // are unavailable in unpackaged apps. By setting them on Application.Resources
        // first, XamlControlsResources finds them during its own initialization.
        // WinUI 3 theme resources — requires MSIX packaging for Acrylic support
        Resources(muxc::XamlControlsResources());

        m_window = xaml::Window();

        auto root = muxc::Grid();
        root.RequestedTheme(xaml::ElementTheme::Dark);
        root.Background(media::SolidColorBrush(
            winrt::Microsoft::UI::ColorHelper::FromArgb(255, 30, 30, 30)));

        auto tabView = muxc::TabView();
        tabView.IsAddTabButtonVisible(true);
        tabView.TabWidthMode(muxc::TabViewWidthMode::Equal);

        auto tab1 = muxc::TabViewItem();
        tab1.Header(winrt::box_value(L"Terminal"));
        tab1.IsClosable(true);
        tabView.TabItems().Append(tab1);
        tabView.SelectedItem(tab1);

        tabView.AddTabButtonClick([](muxc::TabView const& tv, auto&&) {
            auto newTab = muxc::TabViewItem();
            newTab.Header(winrt::box_value(L"Terminal"));
            newTab.IsClosable(true);
            tv.TabItems().Append(newTab);
            tv.SelectedItem(newTab);
        });

        root.Children().Append(tabView);
        m_window.Content(root);
        m_window.Activate();
    }

    winrt::Microsoft::UI::Xaml::Window m_window{ nullptr };
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

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // MSIX packaged app — Windows App Runtime is a package dependency,
    // no MddBootstrapInitialize needed.

    try {
        winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) {
            auto app = winrt::make<GhosttyApp>();
        });
    } catch (winrt::hresult_error const& e) {
        wchar_t buf[512];
        swprintf_s(buf, L"XAML Start failed: 0x%08X\n%s",
            static_cast<unsigned int>(e.code()), e.message().c_str());
        MessageBoxW(nullptr, buf, L"Error", MB_OK);
    }

    return 0;
}
