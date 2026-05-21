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

        // WM_GETMINMAXINFO subclass proc installed lazily on first
        // SIZE_LIMIT. dwRefData carries the MainWindow*; the proc
        // reads m_sizeLimit and clamps ptMin/MaxTrackSize before
        // forwarding to DefSubclassProc.
        static LRESULT CALLBACK SizeLimitSubclassProc(
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

        // Handle GHOSTTY_ACTION_TOGGLE_SPLIT_ZOOM: if no leaf is
        // currently zoomed in the source surface's tab, expand the
        // source leaf to fill the panel; if a leaf is already zoomed
        // there, restore the regular split layout. UI thread only.
        void ToggleSplitZoomForSurface(ghostty_surface_t surface);

        std::unique_ptr<GhosttyApp> m_ghostty;
        HWND m_hwnd = nullptr;
        // Initial window size from GHOSTTY_ACTION_INITIAL_SIZE (physical
        // pixels). Zero means "not yet received" — RESET_WINDOW_SIZE
        // falls back to a DPI-scaled 1280x720 in that case.
        uint32_t m_initialWidth = 0;
        uint32_t m_initialHeight = 0;
        // Glyph cell dimensions (pixels) from GHOSTTY_ACTION_CELL_SIZE.
        // Updated whenever ghostty's font / cell metrics change so any
        // future host-side sizing logic that wants whole-cell rounding
        // (e.g., snap-to-cell resize feedback) has the current value.
        uint32_t m_cellWidth = 0;
        uint32_t m_cellHeight = 0;
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
        PaneIdAllocator m_paneIds;
        Tabs m_tabs;
        // Focus-tracked active surface. Set by NotifySurfaceFocused
        // when a TerminalControl gains focus, cleared when the
        // matching surface is torn down through CloseSurfaceByPaneId.
        ghostty_surface_t m_activeSurface = nullptr;
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
