#pragma once

#include "MainWindow.g.h"
#include "ghostty.h"
#include "Ghostty/Actions/Tags/Fullscreen.h"
#include "Ghostty/Actions/Tags/SizeLimit.h"
#include "Ghostty/Actions/Tags/WindowDecorations.h"
#include "Ghostty/App.h"
#include "Ghostty/CallbackDispatcher.h"
#include "Host/IWindow.h"
#include "Interop/Encoding.h"
#include "Win32/Clipboard.h"
#include "Tabs/Panes/PaneIdAllocator.h"
#include "Tabs/Tab.h"
#include "Tabs/TabFactory.h"
#include "Tabs/Tabs.h"

namespace winrt::GhosttyWin32::implementation
{
    // Shorthand aliases for the Core namespaces — the full
    // winrt::GhosttyWin32::implementation::core::* paths are
    // accurate but unreadable at every member declaration.
    namespace ghostty = core::ghostty;
    namespace host    = core::host;
    namespace interop = core::interop;
    namespace win32   = core::win32;

    struct MainWindow : MainWindowT<MainWindow>, host::IWindow
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
        void Dispatch(std::function<void()> fn) override;
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

        // Tab lifecycle / navigation / title operations from
        // IMainWindowView. Same shape as the split overrides: the
        // tree / TabView live on this class so the body stays here;
        // dispatcher reaches them through the interface.
        void CreateTab() override;
        void CloseTabBySurface(ghostty_surface_t surface) override;
        void GoToTab(int requested) override;
        void SetTabTitleForSurface(ghostty_surface_t surface,
                                   std::wstring title) override;
        void CopyTabTitleForSurface(ghostty_surface_t surface) override;
        void MoveActiveTabBy(ssize_t amount) override;

        // State-owner delegating overrides. Each is a one-liner;
        // the actual logic lives in the dedicated value (m_sizeLimit,
        // m_fullscreen) so MainWindow doesn't accrete fields
        // that nothing outside one specific handler reads.
        void ApplySizeLimit(ghostty_action_size_limit_s limit) override;
        void ToggleFullscreen() override;
        void ToggleWindowDecorations() override;
        // Apply the current decoration state (config + any override
        // installed by ToggleWindowDecorations) to the XAML caption
        // buttons / drag region. Called once at startup so the config
        // value is honoured even without a toggle, and re-called from
        // ToggleWindowDecorations after flipping the tag.
        void ApplyWindowDecorationsAppearance();
        void PresentTerminal() override;
        void ShowOnScreenKeyboard() override;

        // Terminal-driven appearance / lifecycle overrides. Bodies
        // are in MainWindow.xaml.cpp; the logic moved verbatim
        // from the old inline action_cb chunks.
        void ApplyBackgroundColor(uint8_t r, uint8_t g, uint8_t b) override;
        void SetCursorShapeForSurface(ghostty_surface_t surface,
                                      ghostty_action_mouse_shape_e shape) override;
        void ReplaceConfig(ghostty_config_t cloned) override;
        void ReloadConfig(bool soft) override;
        void ShowDesktopNotification(ghostty_surface_t surface,
                                     std::wstring title,
                                     std::wstring body) override;

        // Notification-click entry point. Called by App's
        // AppInstance::Activated handler after a desktop notification
        // is clicked: locate the originating pane (when known), make
        // its tab active, then bring the window forward. Falls back
        // to a plain foreground if `id` is the zero sentinel.
        void PresentNotification(PaneId id);
        void ReportProgress(ghostty_action_progress_report_s pr) override;

    private:
        void InitGhostty();
        Tab* ActiveTab();
        // Convenience wrapper around ActiveTab()->ActiveControl(). Most
        // input/IME paths only care about the focused TerminalControl,
        // not the surrounding Tab — this skips the double deref.
        TerminalControl* ActiveControl();
        // Swaps MaximizeGlyph between Maximize (E922) and Restore (E923)
        // depending on the current OverlappedPresenter state.
        void UpdateMaximizeGlyph();

        // Reconcile AppContent child Visibility with the currently
        // active tab — selected tab's SplitPanel goes Visible, every
        // other tab's panel goes Collapsed. Called from
        // TabView.SelectionChanged on every selection flip (incl. the
        // deferred first-tab activation that runs after ghostty presents
        // its first frame).
        void UpdateActivePanelVisibility();
        // Unparent `tab`'s SplitPanel from AppContent. Called by the
        // close paths just before the Tab object is destroyed so the
        // panel doesn't leak as an orphan child. ~Tab can't do this
        // itself because Tab is deliberately unaware of AppContent.
        void RemoveTabPanelFromAppContent(Tab const& tab);
        // Publish a single drag rectangle to AppWindowTitleBar covering
        // the DragRegion's current bounds. Called from
        // DragRegion.SizeChanged so the rect tracks the strip's free
        // space as tabs are added / removed. Avoids using
        // Window.SetTitleBar (which would mark the whole AppTitleBar —
        // including tab headers — as OS title-bar input, triggering a
        // double-click maximize when the user double-clicks a tab).
        void UpdateDragRectangles();

        // Tear down the pane carrying `id` and update the tree / tab
        // list. Dispatched from close_surface_cb. UI thread only.
        void CloseSurfaceByPaneId(PaneId id);

        std::unique_ptr<ghostty::App> m_ghostty;
        HWND m_hwnd = nullptr;
        // SIZE_LIMIT / TOGGLE_FULLSCREEN state. Default constructed
        // (no limit set, not in fullscreen). Subclasses installed
        // by SizeLimit are auto-removed by Win32 when m_hwnd is
        // destroyed, so no explicit teardown ordering is needed.
        ghostty::actions::tags::SizeLimit          m_sizeLimit;
        ghostty::actions::tags::Fullscreen         m_fullscreen;
        ghostty::actions::tags::WindowDecorations  m_windowDecorations;
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
        // the ghostty::App handle is available; destroyed before
        // m_ghostty so handlers can't observe a half-torn-down app
        // on shutdown.
        std::unique_ptr<ghostty::CallbackDispatcher> m_ghosttyDispatcher;
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
