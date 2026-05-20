#pragma once

#include "MainWindow.g.h"
#include "ghostty.h"
#include "GhosttyApp.h"
#include "PaneIdAllocator.h"
#include "Tab.h"
#include "TabFactory.h"
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

        // Handle GHOSTTY_ACTION_NEW_SPLIT: locate the source pane for
        // `surface`, create a new TerminalControl + ghostty surface,
        // and insert it next to the source according to `direction`.
        // The new pane becomes the active leaf and takes focus. UI
        // thread only.
        void SplitActivePane(ghostty_surface_t surface,
                             ghostty_action_split_direction_e direction);

        // Tear down the pane carrying `id` and update the tree / tab
        // list. Dispatched from close_surface_cb. UI thread only.
        void CloseSurfaceByPaneId(PaneId id);

        // Handle GHOSTTY_ACTION_RESIZE_SPLIT: walk up from the active
        // pane to the nearest ancestor split whose axis matches the
        // direction, then nudge that split's ratio by `amount` DIPs
        // in the requested direction. UI thread only.
        void ResizeSplitFromAction(ghostty_surface_t surface,
                                   ghostty_action_resize_split_s resize);

        // Handle GHOSTTY_ACTION_GOTO_SPLIT: move focus to another
        // pane in the same tab. PREVIOUS/NEXT cycle the tree in
        // depth-first order; UP/DOWN/LEFT/RIGHT pick the leaf whose
        // arranged rect is adjacent in that direction. UI thread only.
        void GotoSplitFromAction(ghostty_surface_t surface,
                                 ghostty_action_goto_split_e direction);

        // Handle GHOSTTY_ACTION_EQUALIZE_SPLITS: reset every split
        // ratio in the active tab to 0.5 so each pane occupies an
        // even share of its parent split. UI thread only.
        void EqualizeSplitsForSurface(ghostty_surface_t surface);

        std::unique_ptr<GhosttyApp> m_ghostty;
        HWND m_hwnd = nullptr;
        PaneIdAllocator m_paneIds;
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
