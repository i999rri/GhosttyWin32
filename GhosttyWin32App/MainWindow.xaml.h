#pragma once

#include "MainWindow.g.h"
#include "ghostty.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <commctrl.h>
#include <vector>

namespace winrt::GhosttyWin32::implementation
{
    struct TabSession {
        ghostty_surface_t surface = nullptr;
        IDXGISwapChain1* swapChain = nullptr;
        Microsoft::UI::Xaml::Controls::SwapChainPanel panel{ nullptr };
    };

    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

    private:
        void InitGhostty();
        void CreateTab();
        TabSession* ActiveSession();

        static LRESULT CALLBACK InputSubclass(HWND hwnd, UINT msg, WPARAM wParam,
            LPARAM lParam, UINT_PTR id, DWORD_PTR refData);

        ghostty_app_t m_app = nullptr;
        ghostty_config_t m_config = nullptr;
        ID3D11Device* m_d3dDevice = nullptr;
        HWND m_hwnd = nullptr;
        std::vector<std::unique_ptr<TabSession>> m_sessions;
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
