#include "framework.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <MddBootstrap.h>
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

        // Define TabView resources manually since XamlControlsResources
        // fails with AcrylicBackgroundFillColorDefaultBrush in unpackaged apps.
        auto res = xaml::ResourceDictionary();
        auto transparent = media::SolidColorBrush(winrt::Microsoft::UI::Colors::Transparent());
        auto subtle = media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(0x0F, 0xFF, 0xFF, 0xFF));
        auto pressed = media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(0x06, 0xFF, 0xFF, 0xFF));
        auto tabBg = media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 32, 32, 32));
        auto tabBgHover = media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 45, 45, 45));
        auto tabBgSelected = media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 50, 50, 50));
        auto textBrush = media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 200, 200, 200));

        // TabView button resources
        res.Insert(winrt::box_value(L"TabViewScrollButtonBackground"), transparent);
        res.Insert(winrt::box_value(L"TabViewButtonBackground"), transparent);
        res.Insert(winrt::box_value(L"TabViewButtonBackgroundPointerOver"), subtle);
        res.Insert(winrt::box_value(L"TabViewButtonBackgroundPressed"), pressed);
        res.Insert(winrt::box_value(L"TabViewButtonForeground"), textBrush);

        // TabView item resources
        res.Insert(winrt::box_value(L"TabViewItemHeaderBackground"), tabBg);
        res.Insert(winrt::box_value(L"TabViewItemHeaderBackgroundPointerOver"), tabBgHover);
        res.Insert(winrt::box_value(L"TabViewItemHeaderBackgroundSelected"), tabBgSelected);
        res.Insert(winrt::box_value(L"TabViewItemHeaderBackgroundPressed"), tabBg);
        res.Insert(winrt::box_value(L"TabViewItemHeaderForeground"), textBrush);
        res.Insert(winrt::box_value(L"TabViewItemHeaderForegroundSelected"), textBrush);
        res.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonForeground"), textBrush);

        // Acrylic fallbacks (referenced by TabView's generic.xaml)
        res.Insert(winrt::box_value(L"AcrylicBackgroundFillColorDefaultBrush"), tabBg);
        res.Insert(winrt::box_value(L"AcrylicBackgroundFillColorBaseBrush"), tabBg);

        // TabView background
        res.Insert(winrt::box_value(L"TabViewBackground"), tabBg);

        // Scroll button foreground
        res.Insert(winrt::box_value(L"TabViewScrollButtonForeground"), textBrush);

        // Corner radius (used by many WinUI controls)
        res.Insert(winrt::box_value(L"ControlCornerRadius"),
            winrt::box_value(xaml::CornerRadiusHelper::FromRadii(4, 4, 4, 4)));
        res.Insert(winrt::box_value(L"OverlayCornerRadius"),
            winrt::box_value(xaml::CornerRadiusHelper::FromRadii(8, 8, 8, 8)));

        // Font sizes
        res.Insert(winrt::box_value(L"TabViewItemScrollButonFontSize"), winrt::box_value(12.0));
        res.Insert(winrt::box_value(L"TabViewItemAddButtonFontSize"), winrt::box_value(12.0));
        res.Insert(winrt::box_value(L"TabViewItemHeaderFontSize"), winrt::box_value(12.0));
        res.Insert(winrt::box_value(L"TabViewItemCloseButtonFontSize"), winrt::box_value(10.0));

        // Dimensions
        res.Insert(winrt::box_value(L"TabViewItemScrollButtonWidth"), winrt::box_value(28.0));
        res.Insert(winrt::box_value(L"TabViewItemScrollButtonHeight"), winrt::box_value(28.0));
        res.Insert(winrt::box_value(L"TabViewItemAddButtonWidth"), winrt::box_value(28.0));
        res.Insert(winrt::box_value(L"TabViewItemAddButtonHeight"), winrt::box_value(28.0));
        res.Insert(winrt::box_value(L"TabViewItemHeaderMinWidth"), winrt::box_value(100.0));
        res.Insert(winrt::box_value(L"TabViewItemHeaderMaxWidth"), winrt::box_value(240.0));
        res.Insert(winrt::box_value(L"TabViewItemHeaderMinHeight"), winrt::box_value(28.0));
        res.Insert(winrt::box_value(L"TabViewItemHeaderMaxHeight"), winrt::box_value(36.0));
        res.Insert(winrt::box_value(L"TabViewItemCloseButtonSize"), winrt::box_value(16.0));
        res.Insert(winrt::box_value(L"TabViewItemScrollButtonPadding"),
            winrt::box_value(xaml::ThicknessHelper::FromUniformLength(0)));
        res.Insert(winrt::box_value(L"TabViewItemAddButtonMargin"),
            winrt::box_value(xaml::ThicknessHelper::FromUniformLength(0)));
        res.Insert(winrt::box_value(L"TabViewHeaderPadding"),
            winrt::box_value(xaml::ThicknessHelper::FromLengths(8, 0, 0, 0)));
        res.Insert(winrt::box_value(L"TabViewItemHeaderPadding"),
            winrt::box_value(xaml::ThicknessHelper::FromLengths(12, 0, 10, 0)));
        res.Insert(winrt::box_value(L"TabViewItemHeaderMargin"),
            winrt::box_value(xaml::ThicknessHelper::FromLengths(0, 4, 0, 0)));
        res.Insert(winrt::box_value(L"TabViewItemSeparatorMargin"),
            winrt::box_value(xaml::ThicknessHelper::FromLengths(0, 6, 0, 6)));
        res.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonMargin"),
            winrt::box_value(xaml::ThicknessHelper::FromLengths(4, 0, 0, 0)));
        res.Insert(winrt::box_value(L"TabViewBorderThickness"),
            winrt::box_value(xaml::ThicknessHelper::FromUniformLength(0)));

        // More button/scroll resources
        res.Insert(winrt::box_value(L"TabViewScrollButtonBackgroundPointerOver"), subtle);
        res.Insert(winrt::box_value(L"TabViewScrollButtonBackgroundPressed"), pressed);
        res.Insert(winrt::box_value(L"TabViewScrollButtonForegroundPointerOver"), textBrush);
        res.Insert(winrt::box_value(L"TabViewScrollButtonForegroundPressed"), textBrush);
        res.Insert(winrt::box_value(L"TabViewButtonForegroundPointerOver"), textBrush);
        res.Insert(winrt::box_value(L"TabViewButtonForegroundPressed"), textBrush);
        res.Insert(winrt::box_value(L"TabViewButtonForegroundDisabled"), transparent);

        // Separator
        res.Insert(winrt::box_value(L"TabViewItemSeparator"), subtle);
        res.Insert(winrt::box_value(L"TabViewItemSeparatorWidth"), winrt::box_value(1.0));

        // Item close button states
        res.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBackground"), transparent);
        res.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBackgroundPointerOver"), subtle);
        res.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonBackgroundPressed"), pressed);
        res.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonForegroundPointerOver"), textBrush);
        res.Insert(winrt::box_value(L"TabViewItemHeaderCloseButtonForegroundPressed"), textBrush);

        // Item header states (more)
        res.Insert(winrt::box_value(L"TabViewItemHeaderForegroundPointerOver"), textBrush);
        res.Insert(winrt::box_value(L"TabViewItemHeaderForegroundPressed"), textBrush);
        res.Insert(winrt::box_value(L"TabViewItemHeaderForegroundDisabled"),
            media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(100, 200, 200, 200)));
        res.Insert(winrt::box_value(L"TabViewItemHeaderBackgroundDisabled"), tabBg);

        // Item corner radius
        res.Insert(winrt::box_value(L"TabViewItemHeaderCornerRadius"),
            winrt::box_value(xaml::CornerRadiusHelper::FromRadii(8, 8, 0, 0)));

        // Border
        res.Insert(winrt::box_value(L"TabViewBorderBrush"), transparent);
        res.Insert(winrt::box_value(L"TabViewItemHeaderBorderBrush"), transparent);
        res.Insert(winrt::box_value(L"TabViewItemHeaderBorderThickness"),
            winrt::box_value(xaml::ThicknessHelper::FromUniformLength(0)));

        // Button border
        res.Insert(winrt::box_value(L"TabViewButtonBorderThickness"),
            winrt::box_value(xaml::ThicknessHelper::FromUniformLength(0)));
        res.Insert(winrt::box_value(L"TabViewButtonBorderBrush"), transparent);
        res.Insert(winrt::box_value(L"TabViewButtonCornerRadius"),
            winrt::box_value(xaml::CornerRadiusHelper::FromRadii(4, 4, 4, 4)));

        // Shadow / elevation
        res.Insert(winrt::box_value(L"TabViewShadow"), winrt::box_value(L""));

        Resources(res);

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

    PACKAGE_VERSION minVer{};
    minVer.Major = 1;
    minVer.Minor = 8;
    if (FAILED(MddBootstrapInitialize(0x00010008, nullptr, minVer))) {
        MessageBoxW(nullptr, L"Windows App Runtime not found.\nInstall from https://aka.ms/windowsappsdk", L"Error", MB_OK);
        return 1;
    }

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

    MddBootstrapShutdown();
    return 0;
}
