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
        // Get the HWND of this WinUI window
        HWND GetWindowHandle();

        // Create a Win32 child window for ghostty rendering
        HWND CreateTerminalWindow(HWND parent);
        static LRESULT CALLBACK TerminalWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        // Action handler
        bool HandleAction(ghostty_target_s target, ghostty_action_s action);

        // Sync popup terminal window position/size with WinUI window
        void UpdateTerminalPosition();

        GhosttyApp m_ghosttyApp;
        ghostty_surface_t m_surface = nullptr;
        HWND m_terminalHwnd = nullptr;

        // Subclass WinUI HWND to catch WM_MOVE
        static LRESULT CALLBACK MainWndSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);

        // Window state
        bool m_fullscreen = false;
        RECT m_savedRect = {};
        DWORD m_savedStyle = 0;
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
