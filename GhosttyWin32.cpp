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

        // Define all TabView theme resources manually since XamlControlsResources
        // cannot be loaded in unpackaged apps (AcrylicBackgroundFillColorDefaultBrush fails).
        auto res = xaml::ResourceDictionary();
        auto K = [](const wchar_t* k) { return winrt::box_value(k); };
        auto C = [](uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
            return media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(a, r, g, b));
        };
        auto T = [](double l, double t, double r, double b) {
            return winrt::box_value(xaml::ThicknessHelper::FromLengths(l, t, r, b));
        };
        auto TU = [](double v) { return winrt::box_value(xaml::ThicknessHelper::FromUniformLength(v)); };
        auto CR = [](double tl, double tr, double br, double bl) {
            return winrt::box_value(xaml::CornerRadiusHelper::FromRadii(tl, tr, br, bl));
        };
        auto D = [](double v) { return winrt::box_value(v); };

        auto transparent = C(0, 0, 0, 0);
        auto subtle      = C(0x0F, 0xFF, 0xFF, 0xFF);
        auto pressed     = C(0x06, 0xFF, 0xFF, 0xFF);
        auto tabBg       = C(255, 32, 32, 32);
        auto tabBgHover  = C(255, 45, 45, 45);
        auto tabBgSel    = C(255, 50, 50, 50);
        auto fg          = C(255, 200, 200, 200);
        auto fgDim       = C(100, 200, 200, 200);

        // === Brush resources (65) ===
        res.Insert(K(L"TabViewBackground"), tabBg);
        res.Insert(K(L"TabViewItemHeaderBackground"), tabBg);
        res.Insert(K(L"TabViewItemHeaderBackgroundSelected"), tabBgSel);
        res.Insert(K(L"TabViewItemHeaderDragBackground"), tabBgHover);
        res.Insert(K(L"TabViewItemHeaderBackgroundPointerOver"), tabBgHover);
        res.Insert(K(L"TabViewItemHeaderBackgroundPressed"), tabBg);
        res.Insert(K(L"TabViewItemHeaderBackgroundDisabled"), tabBg);
        res.Insert(K(L"TabViewItemHeaderForeground"), fg);
        res.Insert(K(L"TabViewItemHeaderForegroundPressed"), fg);
        res.Insert(K(L"TabViewItemHeaderForegroundSelected"), fg);
        res.Insert(K(L"TabViewItemHeaderForegroundPointerOver"), fg);
        res.Insert(K(L"TabViewItemHeaderForegroundDisabled"), fgDim);
        res.Insert(K(L"TabViewItemIconForeground"), fg);
        res.Insert(K(L"TabViewItemIconForegroundPressed"), fg);
        res.Insert(K(L"TabViewItemIconForegroundSelected"), fg);
        res.Insert(K(L"TabViewItemIconForegroundPointerOver"), fg);
        res.Insert(K(L"TabViewItemIconForegroundDisabled"), fgDim);
        res.Insert(K(L"TabViewButtonBackground"), transparent);
        res.Insert(K(L"TabViewButtonBackgroundPressed"), pressed);
        res.Insert(K(L"TabViewButtonBackgroundPointerOver"), subtle);
        res.Insert(K(L"TabViewButtonBackgroundDisabled"), transparent);
        res.Insert(K(L"TabViewButtonForeground"), fg);
        res.Insert(K(L"TabViewButtonForegroundPressed"), fg);
        res.Insert(K(L"TabViewButtonForegroundPointerOver"), fg);
        res.Insert(K(L"TabViewButtonForegroundDisabled"), fgDim);
        res.Insert(K(L"TabViewButtonBorderBrush"), transparent);
        res.Insert(K(L"TabViewButtonBorderBrushPressed"), transparent);
        res.Insert(K(L"TabViewButtonBorderBrushPointerOver"), transparent);
        res.Insert(K(L"TabViewButtonBorderBrushDisabled"), transparent);
        res.Insert(K(L"TabViewScrollButtonBackground"), transparent);
        res.Insert(K(L"TabViewScrollButtonBackgroundPressed"), pressed);
        res.Insert(K(L"TabViewScrollButtonBackgroundPointerOver"), subtle);
        res.Insert(K(L"TabViewScrollButtonBackgroundDisabled"), transparent);
        res.Insert(K(L"TabViewScrollButtonForeground"), fg);
        res.Insert(K(L"TabViewScrollButtonForegroundPressed"), fg);
        res.Insert(K(L"TabViewScrollButtonForegroundPointerOver"), fg);
        res.Insert(K(L"TabViewScrollButtonForegroundDisabled"), fgDim);
        res.Insert(K(L"TabViewScrollButtonBorderBrush"), transparent);
        res.Insert(K(L"TabViewScrollButtonBorderBrushPressed"), transparent);
        res.Insert(K(L"TabViewScrollButtonBorderBrushPointerOver"), transparent);
        res.Insert(K(L"TabViewScrollButtonBorderBrushDisabled"), transparent);
        res.Insert(K(L"TabViewItemSeparator"), subtle);
        res.Insert(K(L"TabViewItemHeaderCloseButtonBackground"), transparent);
        res.Insert(K(L"TabViewItemHeaderCloseButtonBackgroundPressed"), pressed);
        res.Insert(K(L"TabViewItemHeaderCloseButtonBackgroundPointerOver"), subtle);
        res.Insert(K(L"TabViewItemHeaderPressedCloseButtonBackground"), transparent);
        res.Insert(K(L"TabViewItemHeaderPointerOverCloseButtonBackground"), transparent);
        res.Insert(K(L"TabViewItemHeaderSelectedCloseButtonBackground"), transparent);
        res.Insert(K(L"TabViewItemHeaderDisabledCloseButtonBackground"), transparent);
        res.Insert(K(L"TabViewItemHeaderCloseButtonForeground"), fg);
        res.Insert(K(L"TabViewItemHeaderCloseButtonForegroundPressed"), fg);
        res.Insert(K(L"TabViewItemHeaderCloseButtonForegroundPointerOver"), fg);
        res.Insert(K(L"TabViewItemHeaderPressedCloseButtonForeground"), fg);
        res.Insert(K(L"TabViewItemHeaderPointerOverCloseButtonForeground"), fg);
        res.Insert(K(L"TabViewItemHeaderSelectedCloseButtonForeground"), fg);
        res.Insert(K(L"TabViewItemHeaderDisabledCloseButtonForeground"), fgDim);
        res.Insert(K(L"TabViewItemHeaderCloseButtonBorderBrush"), transparent);
        res.Insert(K(L"TabViewItemHeaderCloseButtonBorderBrushPointerOver"), transparent);
        res.Insert(K(L"TabViewItemHeaderCloseButtonBorderBrushPressed"), transparent);
        res.Insert(K(L"TabViewItemHeaderCloseButtonBorderBrushSelected"), transparent);
        res.Insert(K(L"TabViewItemHeaderCloseButtonBorderBrushDisabled"), transparent);
        res.Insert(K(L"TabViewButtonBackgroundActiveTab"), transparent);
        res.Insert(K(L"TabViewButtonForegroundActiveTab"), fg);
        res.Insert(K(L"TabViewBorderBrush"), transparent);
        res.Insert(K(L"TabViewItemBorderBrush"), transparent);
        res.Insert(K(L"TabViewSelectedItemBorderBrush"), transparent);
        // Acrylic fallbacks
        res.Insert(K(L"AcrylicBackgroundFillColorDefaultBrush"), tabBg);
        res.Insert(K(L"AcrylicBackgroundFillColorBaseBrush"), tabBg);

        // === Thickness resources (17) ===
        res.Insert(K(L"TabViewHeaderPadding"), T(8, 0, 0, 0));
        res.Insert(K(L"TabViewItemHeaderPadding"), T(12, 0, 10, 0));
        res.Insert(K(L"TabViewSelectedItemHeaderPadding"), T(12, 0, 10, 0));
        res.Insert(K(L"TabViewButtonBorderThickness"), TU(0));
        res.Insert(K(L"TabViewItemHeaderCloseButtonBorderThickness"), TU(0));
        res.Insert(K(L"TabViewItemHeaderIconMargin"), T(0, 0, 8, 0));
        res.Insert(K(L"TabViewItemHeaderCloseMargin"), T(4, 0, 0, 0));
        res.Insert(K(L"TabViewItemHeaderPaddingWithCloseButton"), T(12, 0, 4, 0));
        res.Insert(K(L"TabViewItemHeaderPaddingWithoutCloseButton"), T(12, 0, 10, 0));
        res.Insert(K(L"TabViewItemScrollButtonPadding"), TU(0));
        res.Insert(K(L"TabViewItemLeftScrollButtonContainerPadding"), TU(0));
        res.Insert(K(L"TabViewItemRightScrollButtonContainerPadding"), TU(0));
        res.Insert(K(L"TabViewItemAddButtonContainerPadding"), TU(0));
        res.Insert(K(L"TabViewItemSeparatorMargin"), T(0, 6, 0, 6));
        res.Insert(K(L"TabViewItemBorderThickness"), TU(1));
        res.Insert(K(L"TabViewSelectedItemBorderThickness"), TU(1));
        res.Insert(K(L"TabViewSelectedItemHeaderMargin"), T(0, 0, 0, 0));
        res.Insert(K(L"TabViewBorderThickness"), TU(0));
        res.Insert(K(L"TabViewItemHeaderMargin"), T(0, 4, 0, 0));

        // === Double resources (16) ===
        res.Insert(K(L"TabViewItemMinHeight"), D(28));
        res.Insert(K(L"TabViewItemMaxWidth"), D(240));
        res.Insert(K(L"TabViewItemMinWidth"), D(100));
        res.Insert(K(L"TabViewItemHeaderFontSize"), D(12));
        res.Insert(K(L"TabViewItemHeaderIconSize"), D(16));
        res.Insert(K(L"TabViewItemHeaderCloseButtonHeight"), D(16));
        res.Insert(K(L"TabViewItemHeaderCloseButtonWidth"), D(16));
        res.Insert(K(L"TabViewItemHeaderCloseButtonSize"), D(16));
        res.Insert(K(L"TabViewItemHeaderCloseFontSize"), D(10));
        res.Insert(K(L"TabViewItemScrollButtonWidth"), D(28));
        res.Insert(K(L"TabViewItemScrollButtonHeight"), D(28));
        res.Insert(K(L"TabViewItemScrollButonFontSize"), D(12));
        res.Insert(K(L"TabViewItemAddButtonWidth"), D(28));
        res.Insert(K(L"TabViewItemAddButtonHeight"), D(28));
        res.Insert(K(L"TabViewItemAddButtonFontSize"), D(12));
        res.Insert(K(L"TabViewShadowDepth"), D(16));
        res.Insert(K(L"TabViewItemSeparatorWidth"), D(1));

        // === CornerRadius resources ===
        res.Insert(K(L"ControlCornerRadius"), CR(4, 4, 4, 4));
        res.Insert(K(L"OverlayCornerRadius"), CR(8, 8, 8, 8));
        res.Insert(K(L"TabViewItemHeaderCornerRadius"), CR(8, 8, 0, 0));
        res.Insert(K(L"TabViewButtonCornerRadius"), CR(4, 4, 4, 4));

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
