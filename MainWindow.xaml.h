#pragma once

#include "MainWindow.g.h"
#include "GhosttyApp.h"

namespace winrt::GhosttyWin32::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        void InitializeTerminal();

    private:
        HWND GetWindowHandle();

        // Get ISwapChainPanelNative from the XAML SwapChainPanel
        void* GetSwapChainPanelNative();

        // Input forwarding — SwapChainPanel receives pointer/key events via XAML
        void OnTerminalKeyDown(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e);
        // CharacterReceived replaced by TSF input
        void OnTerminalPointerMoved(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
        void OnTerminalPointerPressed(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
        void OnTerminalPointerReleased(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
        void OnTerminalPointerWheelChanged(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
        void OnTerminalSizeChanged(winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& e);

        // Action handler
        bool HandleAction(ghostty_target_s target, ghostty_action_s action);

        // Ghostty mods from XAML key state
        ghostty_input_mods_e GetMods();

        GhosttyApp m_ghosttyApp;
        ghostty_surface_t m_surface = nullptr;
        IUnknown* m_panelNative = nullptr; // ISwapChainPanelNative*
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
