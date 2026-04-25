#pragma once

#include "MainWindow.g.h"
#include "ghostty.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include "ImeBuffer.h"

namespace winrt::GhosttyWin32::implementation
{
    struct TabSession {
        ghostty_surface_t surface = nullptr;
        ID3D11Device* device = nullptr;
        IDXGISwapChain1* swapChain = nullptr;
        HANDLE surfaceHandle = nullptr;   // DComp surface handle (owned)
        Microsoft::UI::Xaml::Controls::SwapChainPanel panel{ nullptr };
        HANDLE creationThread = nullptr;  // worker thread handle for cancellation
        bool closing = false;             // set when tab close is requested
    };

    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

    private:
        void InitGhostty();
        void CreateTab();
        TabSession* ActiveSession();

        ghostty_app_t m_app = nullptr;
        ghostty_config_t m_config = nullptr;
        HWND m_hwnd = nullptr;
        IDXGIFactory2* m_dxgiFactory = nullptr;
        IDXGIAdapter* m_dxgiAdapter = nullptr;
        winrt::Windows::UI::Text::Core::CoreTextEditContext m_editContext{ nullptr };
        ImeBuffer m_ime;
        std::vector<std::unique_ptr<TabSession>> m_sessions;
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
