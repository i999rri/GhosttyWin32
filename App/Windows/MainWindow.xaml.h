#pragma once

#include "MainWindow.g.h"
#include "ghostty.h"
#include "Ghostty/Actions/Tags/BackgroundOpacity.h"
#include "Ghostty/Actions/Tags/CellSize.h"
#include "Ghostty/Actions/Tags/Fullscreen.h"
#include "Ghostty/Actions/Tags/SizeLimit.h"
#include "Ghostty/Actions/Tags/WindowDecorations.h"
#include "Ghostty/App.h"
#include "Ghostty/CallbackDispatcher.h"
#include "Host/IWindow.h"
#include "Interop/Encoding.h"
#include "Win32/Clipboard.h"
#include "Tabs/Panes/PaneId.h"
#include "Tabs/ParkedTabs.h"
#include "Tabs/Tab.h"
#include "Tabs/TabFactory.h"
#include "Tabs/Tabs.h"
#include "Windows/WindowCloseGate.h"

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
        // focus (wired by TabFactory). Routes the event to the owning
        // Tab so its active-pane state — and with it the per-tab
        // dim overlay — follows keyboard focus. The window keeps no
        // focused-surface cache of its own: "which surface is the
        // user looking at" is answered by ActiveControl(), which reads
        // the Tab state this call keeps current. Public because
        // TerminalControl needs to reach in via the host-supplied
        // callback. UI thread only.
        void NotifySurfaceFocused(ghostty_surface_t surface) noexcept;

        // Whether `surface` is the one the user is looking at: the
        // active pane of the active tab. Derived, never cached — a
        // cache had to be invalidated on every close / tear-out /
        // pane-close path, and each of those was a chance to let it
        // outlive the surface (the old m_activeSurface contract).
        bool IsActiveSurface(ghostty_surface_t surface) noexcept;

        // True when any leaf in this window's tab tree owns `surface`.
        // App::FindWindowForSurface iterates its window list and asks
        // each in turn — the linear cost is fine at the scale a
        // multi-window session will reach in practice.
        bool OwnsSurface(ghostty_surface_t surface) const noexcept;

        // Same shape as OwnsSurface but keyed by PaneId. Used by
        // MainWindows::FindWindowByPaneId to route close_surface_cb —
        // the userdata payload is a globally unique PaneId (App owns
        // the allocator), so exactly one window returns true.
        bool OwnsPane(PaneId id) const noexcept;

        // ----- IMainWindowView -----
        // Narrow surface the callback dispatcher / GhosttyActions
        // consume; see IMainWindowView.h for why these live behind
        // a virtual interface rather than being looked up off
        // MainWindow directly.
        HWND Hwnd() const noexcept override { return m_hwnd; }
        void Dispatch(std::function<void()> fn) override;
        void Tick() override;
        void RequestClose() override;
        void TryClose() override;

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
        // Gate-approved close of a whole tab (tab X, close_tab
        // keybind). Parks it for undo when allowed, otherwise tears
        // it down now. Assumes the caller already ran confirmation.
        void CloseTabByItem(winrt::Microsoft::UI::Xaml::Controls::TabViewItem const& item);
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
        void SetFloatOnTop(ghostty_action_float_window_e mode) override;
        void ToggleBackgroundOpacity() override;
        // Carry out what the BackgroundOpacity tag decides for the
        // current config: window backdrop, DWM alpha, root brush,
        // per-pane underlays. Called from the toggle, from
        // pane-creation funnels (CreateTab / split / adopt) so new
        // panes match the window state, and from
        // ApplyBackgroundColor when the terminal recolours.
        void ApplyBackgroundOpacityAppearance();
        // The tag's verdict for the current config; the one place
        // the config is read for this purpose.
        ghostty::actions::tags::BackgroundOpacity::Appearance
        BackgroundOpacityAppearance() const;
        // Apply the current decoration state (config + any override
        // installed by ToggleWindowDecorations) to the XAML caption
        // buttons / drag region. Called once at startup so the config
        // value is honoured even without a toggle, and re-called from
        // ToggleWindowDecorations after flipping the tag.
        void ApplyWindowDecorationsAppearance();
        void PresentTerminal() override;
        void ShowOnScreenKeyboard() override;

        // Undo/redo of parked tab closes (#151). CloseTabByItem
        // parks instead of destroying when undo-timeout > 0; these
        // restore the newest parked tab / re-close the tab that was
        // most recently restored.
        void Undo() override;
        void Redo() override;

        // Snap-to-cell window resizing (#155): route the CELL_SIZE
        // report to the WM_SIZING snapping tag when the surface
        // belongs to this window.
        void ApplyCellSizeForSurface(ghostty_surface_t surface,
                                     ghostty_action_cell_size_s cell) override;
        // Shared tail of the surface report and the adopt-time
        // re-arm: hand the WM_SIZING snapping tag the metrics and
        // the current window-step-resize gate.
        void ArmCellSnap(ghostty_action_cell_size_s cell);
        // `window-step-resize` as the config says right now; false
        // before ghostty is up. The one place it is read.
        bool WindowStepResizeByConfig() const;

        // Terminal-driven appearance / lifecycle overrides. Bodies
        // are in MainWindow.xaml.cpp; the logic moved verbatim
        // from the old inline action_cb chunks.
        // Surface directory (IWindow::FindSurfaceView): the one lookup
        // through which every surface-targeted action reaches its
        // pane. Replaces fourteen identical relay overrides.
        host::ISurfaceView* FindSurfaceView(ghostty_surface_t surface) override;
        void ApplyBackgroundColor(uint8_t r, uint8_t g, uint8_t b) override;
        void SetPwdForSurface(ghostty_surface_t surface,
                              std::wstring pwd) override;
        void NotifyCommandFinishedForSurface(ghostty_surface_t surface,
                                             int exitCode,
                                             uint64_t durationNs) override;
        void PromptTitleForSurface(ghostty_surface_t surface) override;
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

        // Read HKCU\...\Themes\Personalize\AppsUseLightTheme and forward
        // to core::ghostty::App::SetColorScheme. Fired at first activation
        // and from the WM_SETTINGCHANGE subclass proc in the .cpp — the
        // subclass proc is a plain free function (not a friend), so this
        // has to be reachable through the class's public surface.
        void PushCurrentSystemColorScheme() noexcept;

    private:
        // The MainWindowRuntime implementation of the ghostty runtime
        // callbacks needs to reach into m_ghosttyDispatcher / m_hwnd /
        // ActiveControl() / CloseSurfaceByPaneId() — the wiring back
        // into this window that ghostty asked the host to provide.
        // Friending the runtime keeps those members private to
        // everyone else while documenting the tight coupling.
        friend class MainWindowRuntime;
        // The process-wide SEH handler on App walks every window's
        // tabs to release composition handles before a fatal crash
        // reaches WER. Same pattern as MainWindowRuntime: a specific
        // outside actor that has an intrinsic reason to reach into
        // this window's guts, granted access by name rather than by
        // widening the public surface.
        friend struct App;


        void InitGhostty();
        // Post-gate teardown for CloseSurfaceByPaneId. Runs when the
        // close gate has already asked the user (or determined the
        // surface doesn't need confirmation).
        void RemovePaneByIdApproved(PaneId id);
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
        // Install a subclass on m_hwnd that watches for WM_SETTINGCHANGE
        // events carrying the "ImmersiveColorSet" payload and pushes the
        // updated OS light/dark preference into ghostty. Called once from
        // the one-shot Activated init after m_hwnd is captured.
        void HookSystemThemeSignal() noexcept;
        // WndProc subclass that intercepts WM_CLOSE (Alt+F4, OS-issued
        // close) and routes it through the confirmation gate. See
        // CloseGateSubclassProc.
        void HookCloseGate() noexcept;

    public:
        // Read by CloseGateSubclassProc (free function in the .cpp)
        // so the subclass can honour the bypass without needing a
        // friend declaration for an anonymous-namespace function.
        bool IsCloseGateBypassed() const noexcept { return m_bypassCloseGate; }
    private:

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
        // Goes through the close gate — the actual mutation lives in
        // RemovePaneByIdApproved and only runs on approval.
        void CloseSurfaceByPaneId(PaneId id);

        // The TerminalControl hosting the pane carrying `id`, or null
        // when no tab in this window owns it (already closed, or it
        // lives in a sibling window). Used by MainWindowRuntime to
        // complete surface-scoped callbacks (clipboard) on exactly
        // the surface that issued them.
        TerminalControl* ControlByPaneId(PaneId id) noexcept;

        // ----- tab drag-out / merge (#55 follow-up) -----
        // Mark this window as a drop host BEFORE its first Activated:
        // it will receive a live, already-presenting tab instead of
        // creating one, so the one-shot init skips the initial
        // CreateTab and the pre-first-frame SW_HIDE (the adopted tab
        // has frames to show from the first paint). Called by
        // App::CreateTearOutWindow through the existing friendship.
        void SuppressInitialTab() noexcept { m_suppressInitialTab = true; }

        // Window-scoped state a torn-out tab should keep: the
        // background-opacity mode of the window it came from
        // (upstream macOS keeps it on the controller, which moves
        // with the tab). Called by the drop-outside handler before
        // AdoptTornOutTab, whose final re-apply then paints this
        // window like the source.
        void InheritWindowState(MainWindow const& source) noexcept {
            m_backgroundOpacity = source.m_backgroundOpacity;
        }

        // Take `item`'s Tab out of this window alive: strip entry
        // removed, panel unparented from AppContent, focused-surface
        // cache cleared if it pointed into the tab — but nothing
        // detached or destroyed, so a sibling window can adopt the
        // same Tab. Returns null when this window doesn't own `item`.
        std::unique_ptr<Tab> ReleaseTornOutTab(
            winrt::Microsoft::UI::Xaml::Controls::TabViewItem const& item);

        // Counterpart of ReleaseTornOutTab: insert the tab into this
        // window's strip at `index` (clamped; negative appends),
        // re-point its controls at this window (host HWND + focused
        // callback), parent the panel, select it, and make sure the
        // window is visible. Safe to call before this window's first
        // Activated — the HWND is captured on demand.
        void AdoptTornOutTab(std::unique_ptr<Tab> tab, int32_t index);

        // Close this window once a tear-out leaves it without tabs,
        // matching browser behaviour. Deferred through the dispatcher
        // so teardown never runs inside tear-out event dispatch.
        void CloseIfTornOutEmpty();

        // True when this window's tab strip owns `item`. Used by the
        // merge path to locate the source window of a torn-out tab.
        bool OwnsTabItem(
            winrt::Microsoft::UI::Xaml::Controls::TabViewItem const& item) const noexcept {
            return m_tabs.FindByItem(item) != nullptr;
        }

        bool m_suppressInitialTab{ false };

        // Push renderer-side visibility to every surface in this
        // window (all tabs, all panes). Driven by
        // Window.VisibilityChanged: while hidden/minimized each
        // surface's renderer thread skips draws entirely instead of
        // ticking its blink / safety-net presents. UI thread only.
        void BroadcastOcclusion(bool visible);

        // True when this window's HWND is the OS foreground window.
        // WinUI3's Window.Activated oscillates continuously when
        // multiple windows share one UI thread, so activation-driven
        // work must not trust the event state verbatim — this per-call
        // Win32 query stays stable through the oscillation and always
        // reflects reality without any lifecycle to maintain.
        bool IsForeground() const noexcept {
            return m_hwnd != nullptr && m_hwnd == ::GetForegroundWindow();
        }

        // Last renderer-side focus value forwarded for this window.
        // Forwards are gated on IsForeground() and deduped here —
        // every ghostty .focus message triggers a renderer frame, so
        // redundant sends are not free. Starts true to match ghostty's
        // surface default.
        bool m_rendererFocus{ true };

        // Borrowed pointer into the App-scope core::ghostty::App
        // (owned by `winrt::App::m_ghostty`). Set in MainWindow's
        // constructor — App's OnLaunched creates ghostty BEFORE
        // make<MainWindow>() and aborts on failure, so by the time
        // this MainWindow exists the borrow is guaranteed non-null.
        // App's destructor frees the wrapper AFTER its `window`
        // member has gone (see App.xaml.h member ordering), so the
        // pointer stays valid for this MainWindow's entire lifetime
        // and every method can read it unconditionally.
        //
        // C runtime callbacks (wakeup_cb, action_cb, …) can't reach
        // this member — they're plain C function pointers without
        // capture — and have to go through `App::g_app->Ghostty()`
        // instead.
        core::ghostty::App* m_ghosttyApp{ nullptr };

        HWND m_hwnd = nullptr;
        // Activated fires whenever the window gains focus, but the
        // one-shot setup below (HWND grab, tab-factory construction,
        // first-tab spawn, etc.) is only meaningful on the first
        // activation. Per-instance rather than a function-static
        // bool because that shape leaked across MainWindow instances
        // and left every window after the first stuck in the
        // "already set up" branch with no HWND, no tabs, no terminal.
        bool m_activatedOnce = false;
        // SIZE_LIMIT / TOGGLE_FULLSCREEN state. Default constructed
        // (no limit set, not in fullscreen). Subclasses installed
        // by SizeLimit are auto-removed by Win32 when m_hwnd is
        // destroyed, so no explicit teardown ordering is needed.
        ghostty::actions::tags::SizeLimit          m_sizeLimit;
        ghostty::actions::tags::CellSize           m_cellSize;
        ghostty::actions::tags::Fullscreen         m_fullscreen;
        ghostty::actions::tags::BackgroundOpacity  m_backgroundOpacity;
        ghostty::actions::tags::WindowDecorations  m_windowDecorations;
        Tabs m_tabs;
        // Undo support for tab closes (#151): parked-tab stack,
        // expiry timers, redo bookkeeping — see Tabs/ParkedTabs.h.
        // This window keeps only the XAML effects (tab-strip
        // add/remove, panel visibility, appearance restate, and the
        // expiry teardown callback).
        ParkedTabs m_parkedTabs;
        // Undo support (#151): park `tab` instead of tearing it down
        // when another tab remains and undo-timeout is non-zero.
        // Returns whether it parked; callers add their own extra
        // conditions before asking.
        bool TryParkTab(Tab& tab);
        // The one immediate tab teardown, shared by every close path
        // once parking is ruled out. Keeps the ordering contract in a
        // single place: DetachAll while the panel is still parented →
        // RemoveAt → unparent the panel → destroy the Tab, or
        // RequestClose when it was the last tab.
        void TearDownTab(Tab& tab);
        // Debug-only structural check, run after every operation
        // that parents or unparents a tab's SplitPanel: the panels
        // under AppContent must equal the tabs this window owns —
        // listed in m_tabs plus parked for undo. A miss is an
        // orphan (the pre-#184 shell-exit leak) or a double
        // unparent; breaks into the debugger with the offending
        // path on the stack. No-op in release builds.
        void DebugAssertPanelInvariant() noexcept;
        // Detach the item from the tab strip and move its Tab into
        // m_parkedTabs. fromRedo keeps the redo history intact (a
        // user-initiated close invalidates it).
        void ParkTab(Microsoft::UI::Xaml::Controls::TabViewItem const& item,
                     uint64_t timeoutMs, bool fromRedo);
        // Guards every close intent (window / tab / surface) so
        // needs_confirm_quit prompts land once, not one dialog per
        // path. Constructed inline so it's usable from the ctor.
        WindowCloseGate m_closeGate;
        // Set to true by the gate's approval callback before it calls
        // Window::Close(); the CloseGate WndProc subclass reads this
        // to let the resulting WM_CLOSE (if any) through without
        // re-prompting. One-shot — the window is about to die.
        bool m_bypassCloseGate = false;
        // Re-entrancy guard for the rename-title prompt: WinUI allows
        // one ContentDialog per XamlRoot, and a second ShowAsync while
        // one is up throws. Set when the dialog opens, cleared in its
        // Completed handler.
        bool m_renamePromptOpen = false;
        // Terminal background colour as last applied (config value at
        // init, updated by COLOR_CHANGE via ApplyBackgroundColor).
        // Feeds the opaque underlays and the root brush.
        winrt::Windows::UI::Color m_bgColor{ 255, 0, 0, 0 };
        // Constructed once ghostty is initialized — needs the app handle
        // and HWND, neither available until InitGhostty has run.
        std::unique_ptr<TabFactory> m_tabFactory;
        // ghostty runtime callback dispatcher (today: action_cb;
        // future: clipboard / surface). Built in InitGhostty after
        // the ghostty::App handle is available; the App-scope
        // ghostty wrapper outlives every MainWindow (see App.xaml.h
        // member ordering), so the dispatcher can't observe a
        // half-torn-down ghostty handle from any of its handlers.
        std::unique_ptr<ghostty::CallbackDispatcher> m_ghosttyDispatcher;

        // 1 Hz poll that walks each Tab, asks ghostty for the
        // foreground process pid of the active pane, resolves it to
        // an executable basename, and (if the shell hasn't set an
        // OSC title on this tab yet) writes the name into the
        // TabViewItem header. See StartForegroundPidPoll for the
        // scheduling story.
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer
            m_foregroundPidTimer{ nullptr };

        // Kick off the foreground-pid poll after the first tab
        // exists. Idempotent — safe to call more than once (won't
        // create a second timer).
        void StartForegroundPidPoll();

        // One tick of the poll. Body is inline in the .cpp; walks
        // m_tabs and updates the header where appropriate.
        void UpdateForegroundNames() noexcept;
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
