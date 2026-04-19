#pragma once

#include "MainWindow.g.h"
#include "ghostty.h"
#include <d3d11.h>

namespace winrt::GhosttyWin32::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

    private:
        void InitGhostty();
        void CreateTab();

        ghostty_app_t m_app = nullptr;
        ghostty_config_t m_config = nullptr;
        ID3D11Device* m_d3dDevice = nullptr;
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
