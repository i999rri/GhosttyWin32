#pragma once

#include "MainWindow.g.h"
#include "ghostty.h"
#include "ImeBuffer.h"
#include "Tab.h"
#include <vector>

namespace winrt::GhosttyWin32::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

    private:
        void InitGhostty();
        void CreateTab();
        Tab* ActiveTab();

        ghostty_app_t m_app = nullptr;
        ghostty_config_t m_config = nullptr;
        HWND m_hwnd = nullptr;
        winrt::Windows::UI::Text::Core::CoreTextEditContext m_editContext{ nullptr };
        ImeBuffer m_ime;
        std::vector<std::unique_ptr<Tab>> m_tabs;
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
