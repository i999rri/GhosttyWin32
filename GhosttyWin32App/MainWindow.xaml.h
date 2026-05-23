#pragma once

#include "MainWindow.g.h"
#include "ghostty.h"
#include "GhosttyApp.h"
#include "IMainWindowView.h"
#include "PaneIdAllocator.h"
#include "Tab.h"
#include "TabFactory.h"
#include "Tabs.h"

namespace winrt::GhosttyWin32::implementation
{
    class GhosttyCallbackDispatcher;

    struct MainWindow : MainWindowT<MainWindow>, IMainWindowView
    {
        MainWindow();
        ~MainWindow();

        // Best-effort cleanup invoked from SetUnhandledExceptionFilter.
        // Walks live tabs and closes their composition surface handles so
        // DComp drops its driver-side references before the OS kills the
        // process — reduces the chance the next launch inherits corrupted
        // NVIDIA state.
        static long __stdcall OnUnhandledException(struct _EXCEPTION_POINTERS* info) noexcept;

        // WM_GETMINMAXINFO subclass proc installed lazily on first
        // SIZE_LIMIT. dwRefData carries the MainWindow*; the proc
        // reads m_sizeLimit and clamps ptMin/MaxTrackSize before
        // forwarding to DefSubclassProc.
        static LRESULT CALLBACK SizeLimitSubclassProc(
            HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
            UINT_PTR id, DWORD_PTR ref) noexcept;

        // WM_SETCURSOR subclass proc for MOUSE_VISIBILITY. WinUI 3
        // ignores ShowCursor (the cursor goes through ProtectedCursor
        // / InputSystemCursor instead), so the only reliable hide
        // path is to short-circuit WM_SETCURSOR with SetCursor(NULL)
        // while m_cursorHidden is true.
        static LRESULT CALLBACK CursorVisibilitySubclassProc(
            HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
            UINT_PTR id, DWORD_PTR ref) noexcept;

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

        // Called by every TerminalControl when it receives keyboard
        // focus (wired by TabFactory). Updates m_activeSurface so any
        // future caller — APP-target action handlers, multi-window
        // focus delivery, IPC / scripting bridges — can ask "which
        // surface is the user looking at right now" without a
        // separate tree walk. Public because TerminalControl needs to
        // reach in via the host-supplied callback. UI thread only.
        void NotifySurfaceFocused(ghostty_surface_t surface) noexcept;

        // Last surface to receive keyboard focus inside this window.
        // Null until the first focus delivery (typically the very
        // first tab's TerminalControl GotFocus right after launch).
        // Stays valid across alt-tab — we only clear it when the
        // surface itself is torn down.
        ghostty_surface_t GetActiveSurface() const noexcept { return m_activeSurface; }

        // ----- IMainWindowView -----
        // Narrow surface the callback dispatcher / GhosttyActions
        // consume; see IMainWindowView.h for why these live behind
        // a virtual interface rather than being looked up off
        // MainWindow directly.
        HWND Hwnd() const noexcept override { return m_hwnd; }
        winrt::Microsoft::UI::Dispatching::DispatcherQueue Dispatcher() const override;
        void Tick() override;
        void RequestClose() override;

        // Split-pane operations from IMainWindowView. Bodies are in
        // MainWindow.xaml.cpp; behaviour is unchanged from when these
        // lived as private methods, only the access spec moved so the
        // dispatcher can reach them through the interface.
        void SplitActivePane(ghostty_surface_t surface,
                             ghostty_action_split_direction_e direction) override;
        void ResizeSplitFromAction(ghostty_surface_t surface,
                                   ghostty_action_resize_split_s resize) override;
        void GotoSplitFromAction(ghostty_surface_t surface,
                                 ghostty_action_goto_split_e direction) override;
        void EqualizeSplitsForSurface(ghostty_surface_t surface) override;
        void ToggleSplitZoomForSurface(ghostty_surface_t surface) override;

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

        // Tear down the pane carrying `id` and update the tree / tab
        // list. Dispatched from close_surface_cb. UI thread only.
        void CloseSurfaceByPaneId(PaneId id);

        std::unique_ptr<GhosttyApp> m_ghostty;
        HWND m_hwnd = nullptr;
        // Fullscreen toggle state — TOGGLE_FULLSCREEN flips m_fullscreen
        // and uses m_prevPlacement / m_prevStyle to restore the original
        // window when leaving fullscreen. We save WINDOWPLACEMENT instead
        // of a RECT because it round-trips maximised state correctly
        // (toggling FS from a maximised window should return to maximised).
        bool m_fullscreen = false;
        WINDOWPLACEMENT m_prevPlacement{};
        LONG_PTR m_prevStyle = 0;
        // SIZE_LIMIT — min / max window size in pixels. The values are
        // applied in WM_GETMINMAXINFO via SizeLimitSubclassProc, which
        // is installed lazily on the first SIZE_LIMIT so apps that
        // never set a size limit don't pay the subclass cost.
        ghostty_action_size_limit_s m_sizeLimit{};
        bool m_sizeLimitSubclassed = false;
        // MOUSE_VISIBILITY — when true, WM_SETCURSOR returns NULL so
        // the cursor stays hidden until the next VISIBLE transition.
        // Subclass installed lazily on the first MOUSE_VISIBILITY so
        // the WM_SETCURSOR interception doesn't happen for apps that
        // never fire the action.
        bool m_cursorHidden = false;
        bool m_cursorSubclassed = false;
        PaneIdAllocator m_paneIds;
        Tabs m_tabs;
        // Focus-tracked active surface. Set by NotifySurfaceFocused
        // when a TerminalControl gains focus, cleared when the
        // matching surface is torn down through CloseSurfaceByPaneId.
        ghostty_surface_t m_activeSurface = nullptr;
        // Constructed once ghostty is initialized — needs the app handle
        // and HWND, neither available until InitGhostty has run.
        std::unique_ptr<TabFactory> m_tabFactory;
        // ghostty runtime callback dispatcher (today: action_cb;
        // future: clipboard / surface). Built in InitGhostty after
        // the GhosttyApp handle is available; destroyed before
        // m_ghostty so handlers can't observe a half-torn-down app
        // on shutdown.
        std::unique_ptr<GhosttyCallbackDispatcher> m_ghosttyDispatcher;
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
