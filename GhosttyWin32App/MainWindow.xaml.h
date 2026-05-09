#pragma once

#include "MainWindow.g.h"
#include "ghostty.h"
#include "GhosttyApp.h"
#include "Tab.h"
#include "TabFactory.h"
#include "TabIdAllocator.h"
#include "Tabs.h"

namespace winrt::GhosttyWin32::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        // Best-effort cleanup invoked from SetUnhandledExceptionFilter.
        // Walks live tabs and closes their composition surface handles so
        // DComp drops its driver-side references before the OS kills the
        // process — reduces the chance the next launch inherits corrupted
        // NVIDIA state.
        static long __stdcall OnUnhandledException(struct _EXCEPTION_POINTERS* info) noexcept;

        // Caption button click handlers, referenced from MainWindow.xaml.
        // Routed through Win32 messages (WM_SYSCOMMAND / WM_CLOSE / ShowWindow)
        // rather than OverlappedPresenter state changes, which have
        // historically tripped the NVIDIA driver crash from issue #26.
        void OnMinimizeClick(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMaximizeClick(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCloseClick(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        void InitGhostty();
        void CreateTab();
        Tab* ActiveTab();
        // Convenience wrapper around ActiveTab()->ActiveControl(). Most
        // input/IME paths only care about the focused TerminalControl,
        // not the surrounding Tab — this skips the double deref.
        TerminalControl* ActiveControl();
        // Swaps MaximizeGlyph between Maximize (E922) and Restore (E923)
        // depending on the current OverlappedPresenter state.
        void UpdateMaximizeGlyph();

        std::unique_ptr<GhosttyApp> m_ghostty;
        HWND m_hwnd = nullptr;
        TabIdAllocator m_tabIds;
        Tabs m_tabs;
        // Constructed once ghostty is initialized — needs the app handle
        // and HWND, neither available until InitGhostty has run.
        std::unique_ptr<TabFactory> m_tabFactory;
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
