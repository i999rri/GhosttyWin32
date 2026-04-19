#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace muxc = Microsoft::UI::Xaml::Controls;

namespace winrt::GhosttyWin32::implementation
{
    MainWindow::MainWindow()
    {
        // Extend content into title bar — TabView becomes the title bar
        ExtendsContentIntoTitleBar(true);

        Activated([this](auto&&, auto&&) {
            static bool initialized = false;
            if (initialized) return;
            initialized = true;

            auto tv = TabView();
            // Set TabView as the custom title bar drag region
            SetTitleBar(tv);
            tv.AddTabButtonClick([](muxc::TabView const& sender, auto&&) {
                auto newTab = muxc::TabViewItem();
                newTab.Header(box_value(L"Terminal"));
                newTab.IsClosable(true);
                newTab.Content(muxc::SwapChainPanel());
                sender.TabItems().Append(newTab);
                sender.SelectedItem(newTab);
            });

            tv.TabCloseRequested([this](muxc::TabView const& sender, muxc::TabViewTabCloseRequestedEventArgs const& args) {
                uint32_t idx = 0;
                if (sender.TabItems().IndexOf(args.Tab(), idx)) {
                    sender.TabItems().RemoveAt(idx);
                }
                if (sender.TabItems().Size() == 0) {
                    this->Close();
                }
            });
        });
    }
}
