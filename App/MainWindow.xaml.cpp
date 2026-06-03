#include "pch.h"
#include "MainWindow.xaml.h"
#include "Ghostty/CallbackDispatcher.h"
#include "Host/KeyModifiers.h"
#include "Interop/Encoding.h"
#include "Win32/Clipboard.h"
#include "Win32/SEHGuard.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include <microsoft.ui.xaml.window.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl_core.h>
#include <commctrl.h>
#include <winrt/Microsoft.Windows.AppNotifications.h>
#include <winrt/Microsoft.Windows.AppNotifications.Builder.h>
#include <winrt/Windows.Graphics.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

namespace {
    // Flag file used to detect that the previous process didn't exit cleanly.
    // Created at startup, deleted on clean shutdown — if it's still there at
    // launch time, the previous run crashed and we wait briefly so the
    // NVIDIA driver has time to recover its internal state.
    std::filesystem::path crashFlagPath() {
        wchar_t buf[MAX_PATH];
        DWORD len = GetTempPathW(MAX_PATH, buf);
        if (len == 0) return L"GhosttyWin32_running.flag";
        return std::filesystem::path(buf) / L"GhosttyWin32_running.flag";
    }
}

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace muxc = Microsoft::UI::Xaml::Controls;

static winrt::GhosttyWin32::implementation::MainWindow* g_mainWindow = nullptr;

namespace winrt::GhosttyWin32::implementation
{
    MainWindow::MainWindow()
    {
        ExtendsContentIntoTitleBar(true);

        Activated([this](auto&&, auto&&) {
            static bool initialized = false;
            if (initialized) return;
            initialized = true;

            // Best-effort cleanup if we crash later — tells DComp to release
            // surfaces so the next launch starts cleaner. The crash flag
            // itself is set / checked / cleared in App::OnLaunched so the
            // recovery delay happens before any window is mapped (avoids a
            // visible white flash).
            SetUnhandledExceptionFilter(&MainWindow::OnUnhandledException);

            g_mainWindow = this;
            auto windowNative = this->try_as<::IWindowNative>();
            if (windowNative) windowNative->get_WindowHandle(&m_hwnd);
            if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);

            // Remove the OS title bar (and with it the system caption
            // buttons) so only our XAML CaptionButtons render at the top.
            //
            // We tried two cheaper alternatives first and both failed in
            // WinUI 3 1.8:
            //   * AppWindowTitleBar.Button*Color set to transparent — the
            //     glyphs still rendered.
            //   * Subclassing WM_NCCALCSIZE / WM_NCHITTEST (the Windows
            //     Terminal NonClientIslandWindow pattern) — removes the
            //     Win32 NC frame but the WinUI compositor keeps drawing
            //     the caption buttons via AppWindowTitleBar, and tab
            //     creation re-triggers that draw, leaving multiple sets
            //     stacked on top of each other.
            // Only OverlappedPresenter::SetBorderAndTitleBar reliably
            // tells the presenter "no title bar" so the buttons go away.
            //
            // Issue #26 cautioned against OverlappedPresenter state
            // transitions because dynamically toggling them tripped an
            // NVIDIA driver AV (nvwgf2umx.dll Present). We set it once
            // here, before the first frame renders, so the renderer
            // never sees a transition; the crash flag + 2-second startup
            // recovery in App.cpp remains as a safety net if this ever
            // regresses.
            if (auto presenter = AppWindow().Presenter().try_as<
                    winrt::Microsoft::UI::Windowing::OverlappedPresenter>()) {
                presenter.SetBorderAndTitleBar(true, false);
            }

            // Follow OS theme + Mica backdrop
            {
                auto settings = winrt::Windows::UI::ViewManagement::UISettings();
                auto fg = settings.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Foreground);
                bool isDark = (fg.R > 128);
                Content().as<winrt::Microsoft::UI::Xaml::FrameworkElement>().RequestedTheme(
                    isDark ? winrt::Microsoft::UI::Xaml::ElementTheme::Dark
                           : winrt::Microsoft::UI::Xaml::ElementTheme::Light);
                auto backdrop = winrt::Microsoft::UI::Xaml::Media::MicaBackdrop();
                this->SystemBackdrop(backdrop);
            }

            // On every window (re)activation: pull keyboard focus onto
            // the active terminal and re-attach IME. Two reasons to
            // anchor both jobs on this event:
            //
            //   * Focus on initial show. The first tab's onActivated
            //     callback calls ShowWindow(SW_SHOW), which only posts
            //     WM_ACTIVATE — WinUI's own activation logic runs later
            //     in the message pump and assigns default focus to its
            //     internal first-focusable element. Calling Focus
            //     directly inside onActivated (or through a Low-priority
            //     dispatcher tick) races against that and loses
            //     intermittently. The Activated event itself is signalled
            //     after WinUI has finished its default-focus pass, so a
            //     Focus call here is the last write and reliably sticks.
            //     Subsequent alt-tab returns ride the same path: focus
            //     comes back to the terminal, which is what users expect
            //     of a terminal app where there's nothing else to focus.
            //
            //   * IME re-attach. XAML's GotFocus/LostFocus on
            //     TerminalControl don't fire on window de/activation
            //     (focus is logically retained on the focused element
            //     across alt-tab), so we forward window state changes to
            //     the active control's EditContext directly. Without
            //     this, switching focus to another window and back leaves
            //     the OS-side text-services manager pointing at a
            //     detached EditContext and IME stays off even if the
            //     OS-level IME toggle is on.
            //
            // weak_ref + try/catch instead of `[this]`: WindowActivated
            // fires during shutdown after MainWindow has started
            // disposing — m_tabs is mid-destruction and ActiveControl()
            // can return a dangling TerminalControl pointer. Calling
            // NotifyImeFocusLeave on it AVs at the m_editContext
            // member offset inside microsoft.ui.xaml.dll. The weak_ref
            // path bails cleanly; the catch covers RO_E_CLOSED if
            // TabView() is hit on a torn-down window.
            auto weakActivated = get_weak();
            Activated([weakActivated](winrt::Windows::Foundation::IInspectable const&,
                                      winrt::Microsoft::UI::Xaml::WindowActivatedEventArgs const& args) {
                auto self = weakActivated.get();
                if (!self) return;
                try {
                    using State = winrt::Microsoft::UI::Xaml::WindowActivationState;
                    if (args.WindowActivationState() == State::Deactivated) {
                        if (auto* tc = self->ActiveControl()) {
                            tc->NotifyImeFocusLeave();
                        }
                        // Spurious-deactivation recovery, deferred.
                        // The Win32 title-bar tracking modal loop
                        // DefWindowProc runs for HTCAPTION clicks
                        // briefly steals foreground for tracking
                        // proxies, so a synchronous
                        // GetForegroundWindow() check here reads a
                        // transient non-our-HWND value and
                        // misclassifies the spurious deactivation as
                        // genuine. Bouncing through the dispatcher
                        // delays the check until after the modal loop
                        // returns and foreground state settles. If by
                        // then our HWND is still foreground, we
                        // self-Activate so the activated branch of
                        // this same handler re-runs and queues the
                        // focus restore. Genuine deactivation leaves
                        // foreground on the other app, so the check
                        // skips re-activation and the window properly
                        // backgrounds.
                        auto dq = self->DispatcherQueue();
                        if (dq) {
                            dq.TryEnqueue([weakActivated]() {
                                auto self = weakActivated.get();
                                if (!self || !self->m_hwnd) return;
                                if (GetForegroundWindow() == self->m_hwnd) {
                                    try { self->Activate(); }
                                    catch (winrt::hresult_error const&) {}
                                }
                            });
                        }
                        return;
                    }
                    // Window came back into focus. Restoring focus
                    // inline used to be reliable when each tab had
                    // exactly one focusable TerminalControl, but with
                    // multiple panes WinUI's default-focus pass races
                    // with us and sometimes lands focus on a different
                    // TabStop (a sibling pane, the TabView header,
                    // etc.). Deferring through the DispatcherQueue at
                    // Low priority puts our Focus call after every
                    // default-focus assignment XAML schedules for this
                    // activation, so the last write wins. Same trick
                    // as the SelectionChanged path, which is naturally
                    // last because SelectedItem assignment is itself
                    // dispatcher-scheduled.
                    auto dq = self->DispatcherQueue();
                    if (!dq) return;
                    dq.TryEnqueue(
                        winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
                        [weakActivated]() {
                            auto self = weakActivated.get();
                            if (!self) return;
                            try {
                                if (auto* tab = self->ActiveTab()) {
                                    tab->Focus();
                                }
                                if (auto* tc = self->ActiveControl()) {
                                    tc->NotifyImeFocusEnter();
                                }
                            } catch (winrt::hresult_error const&) {
                            }
                        });
                } catch (winrt::hresult_error const&) {
                }
            });

            auto tv = TabView();
            // Don't call Window.SetTitleBar(AppTitleBar()) — that would
            // make the whole chrome row OS-title-bar input, including the
            // tab headers. Double-clicking a tab header would then
            // satisfy the OS's "double-click on title bar = maximize"
            // gesture, so a quick second click on a tab maximises the
            // window instead of just selecting the tab.
            //
            // Use AppWindowTitleBar.SetDragRectangles instead: explicit
            // physical-pixel rectangles for the drag region. The tabs +
            // caption-button columns are outside any drag rect, so the
            // OS treats clicks on them as ordinary content clicks.
            // UpdateDragRectangles() is the single source of truth and
            // is re-called when DragRegion's bounds change (tab add /
            // remove / TabStripFooter resize) so the rect tracks the
            // strip's free space dynamically.
            DragRegion().SizeChanged([weakSelf = get_weak()](auto&&, auto&&) {
                if (auto self = weakSelf.get()) self->UpdateDragRectangles();
            });
            // Publish the initial rect once the strip has finished its
            // first layout. DragRegion lives inside TabView's template,
            // which only realises on TabView.Loaded — calling
            // UpdateDragRectangles here would publish an empty rect that
            // gets corrected immediately by the SizeChanged above, but
            // the empty rect would leak through to OS until the next
            // resize. Tying it to TabView.Loaded keeps the first
            // published rect already correct.
            tv.Loaded([weakSelf = get_weak()](auto&&, auto&&) {
                if (auto self = weakSelf.get()) self->UpdateDragRectangles();
            });

            // Title-bar click focus restore. Clicking the DragRegion
            // immediately knocks focus off the active TerminalControl
            // (Win32 HTCAPTION click handling moves XAML logical focus
            // into limbo), and the subsequent Activated state goes
            // Deactivated long enough that the foreground check in
            // the activation handler reports a "genuine" deactivation
            // and skips recovery. Bouncing the Focus call through the
            // dispatcher from PointerReleased restores focus right
            // after the click completes, no matter how the activation
            // state ends up.
            auto weakSelfDrag = get_weak();
            DragRegion().PointerReleased([weakSelfDrag](auto&&, auto&&) {
                auto self = weakSelfDrag.get();
                if (!self) return;
                auto dq = self->DispatcherQueue();
                if (!dq) return;
                dq.TryEnqueue([weakSelfDrag]() {
                    auto self = weakSelfDrag.get();
                    if (!self) return;
                    try {
                        if (auto* tab = self->ActiveTab()) {
                            tab->Focus();
                        }
                    } catch (winrt::hresult_error const&) {}
                });
            });

            // Pointer / keyboard / IME routing all live on
            // TerminalControl — each instance hooks the events on
            // itself and forwards directly to its own ghostty surface.
            // No window-level input handler is needed here.

            // DPI / scale handling now lives per-leaf in TerminalControl:
            // each control subscribes to its own
            // SwapChainPanel.CompositionScaleChanged and pushes the
            // current scale into ghostty via
            // ghostty_surface_set_content_scale. The panel's
            // composition scale is what the panel actually composites
            // the swap chain at, so matching that (rather than
            // GetDpiForWindow on the window) keeps the swap chain's
            // render scale and the panel's display scale in sync. On
            // RDP these two values transiently diverge — composition
            // scale starts at 1.0 even when the window DPI is 192,
            // then jumps to the real value once the composition
            // pipeline has run — which is the case that motivated the
            // switch.
            Content().as<winrt::Microsoft::UI::Xaml::FrameworkElement>().Loaded([this](auto&&, auto&&) {
                // Track window state so the maximize button glyph swaps
                // between Maximize (E922) and Restore (E923). DidSizeChange
                // covers the SC_MAXIMIZE / SC_RESTORE round trip we send
                // from the XAML click handlers; DidPresenterChange covers
                // anything that swaps the presenter type.
                AppWindow().Changed([this](auto&&,
                    winrt::Microsoft::UI::Windowing::AppWindowChangedEventArgs const& args) {
                    if (args.DidPresenterChange() || args.DidSizeChange()) {
                        UpdateMaximizeGlyph();
                    }
                });
                UpdateMaximizeGlyph();
            });

            tv.AddTabButtonClick([this](muxc::TabView const&, auto&&) {
                CreateTab();
            });

            // TabView's built-in AddTabButton (the "+") is focusable by
            // default, and its Click cycle holds onto focus across the
            // tab-creation sequence — so even after the new tab is
            // selected and we Focus() the new TerminalControl, the +
            // button retains keyboard focus and the next Enter press
            // re-fires its Click (creating yet another tab). Walking
            // TabView's template to flip IsTabStop/AllowFocusOnInteraction
            // on the AddButton breaks that retention.
            //
            // The template only materialises after Loaded, so we hook
            // TabView.Loaded and walk its visual tree once.
            tv.Loaded([](winrt::Windows::Foundation::IInspectable const& sender, auto&&) {
                auto tv = sender.try_as<muxc::TabView>();
                if (!tv) return;
                namespace mux = winrt::Microsoft::UI::Xaml;
                std::function<bool(mux::DependencyObject const&)> walk =
                    [&walk](mux::DependencyObject const& parent) -> bool {
                        int count = mux::Media::VisualTreeHelper::GetChildrenCount(parent);
                        for (int i = 0; i < count; ++i) {
                            auto child = mux::Media::VisualTreeHelper::GetChild(parent, i);
                            if (auto fe = child.try_as<mux::FrameworkElement>()) {
                                if (fe.Name() == L"AddButton") {
                                    if (auto button = child.try_as<muxc::Button>()) {
                                        button.IsTabStop(false);
                                        button.AllowFocusOnInteraction(false);
                                    }
                                    return true;
                                }
                            }
                            if (walk(child)) return true;
                        }
                        return false;
                    };
                walk(tv);
            });

            tv.TabCloseRequested([this](muxc::TabView const& sender, muxc::TabViewTabCloseRequestedEventArgs const& args) {
                auto item = args.Tab();
                // Detach the control BEFORE removing it from TabView.
                // Detach calls ISwapChainPanelNative2::SetSwapChainHandle(nullptr)
                // on the inner panel, and that call AVs at +0x1F8 inside
                // microsoft.ui.xaml.dll if the panel has already been
                // unparented from the live visual tree (reproducer:
                // Ctrl+Shift+W long-press across multiple tabs, where
                // XAML hasn't finished processing the previous RemoveAt
                // when the next Detach kicks in). Doing it pre-RemoveAt
                // keeps the panel in the live tree for the duration of
                // SetSwapChainHandle. Detach is idempotent, so the
                // ~Tab → ~TerminalControl path runs it again as a no-op.
                if (auto* t = m_tabs.FindByItem(item)) {
                    // Detach every pane in the tab, not just the active
                    // one — multi-pane tabs have multiple swap chains
                    // and each needs SetSwapChainHandle(nullptr) before
                    // the panel is unparented.
                    t->DetachAll();
                }
                uint32_t idx = 0;
                if (sender.TabItems().IndexOf(item, idx)) {
                    sender.TabItems().RemoveAt(idx);
                }
                DwmFlush();              // wait for compositor to release
                if (sender.TabItems().Size() == 0) {
                    // Last tab: defer Tab object destruction to
                    // ~MainWindow's m_tabs.Clear. Tearing down the
                    // focused control synchronously here leaves XAML's
                    // focus subsystem holding a stale pointer that AVs
                    // at +0x1F8 once mw->Close() kicks off window
                    // teardown — same path as a normal title-bar X
                    // close, which works fine precisely because XAML
                    // finishes its own focus cleanup before our
                    // destructors run. The orphan SplitPanel under
                    // AppContent comes down with the window, no
                    // explicit Remove needed.
                    this->Close();
                } else {
                    // Unparent the panel from AppContent before
                    // destroying the Tab. ~Tab doesn't touch the
                    // visual tree (Tab doesn't know about AppContent),
                    // so without this the panel would leak as an
                    // orphan child of AppContent.
                    if (auto* t = m_tabs.FindByItem(item)) {
                        RemoveTabPanelFromAppContent(*t);
                    }
                    m_tabs.Remove(item);
                }
            });

            // Whenever the selected tab changes — explicit click on a
            // header, AddTabButton creating a new tab, keybind switch,
            // auto-reselect after a close — pull focus into the new
            // active TerminalControl. Without this, focus stays on
            // whatever element triggered the selection (most painfully:
            // the AddTabButton, whose IsDefault-like Enter-handling
            // would create yet another tab on the next Enter keystroke).
            // TerminalControl is a UserControl with IsTabStop=true so
            // the Focus call actually moves focus, unlike the bare
            // SwapChainPanel from before the refactor.
            //
            // weak_ref instead of `this`: TabView fires SelectionChanged
            // during shutdown as TabItems is cleared, after MainWindow
            // has started disposing — a raw `this->TabView()` call then
            // throws RO_E_CLOSED on the disposed window.
            auto weakSelf = get_weak();
            tv.SelectionChanged([weakSelf](auto&&, auto&&) {
                auto self = weakSelf.get();
                if (!self) return;
                // weak_ref.get() can return non-null briefly while the
                // window is mid-dispose, in which case TabView() throws
                // RO_E_CLOSED. Swallow — focus restoration is moot then.
                try {
                    // Make the selected tab's SplitPanel the only Visible
                    // child of AppContent. Must run before Focus() — XAML
                    // refuses focus on a Collapsed element, so the new
                    // active panel has to be Visible before tab->Focus()
                    // fires. Old active panel goes Collapsed but stays
                    // parented; the swap chain handle keeps its DComp
                    // binding across the switch.
                    self->UpdateActivePanelVisibility();
                    // Defer Focus to the next dispatcher tick. The
                    // synchronous Focus call races XAML's own focus
                    // migration when the previously-active panel just
                    // went Collapsed — observable as "click a tab once,
                    // nothing happens; click again to actually switch"
                    // because focus stays parked on the now-Collapsed
                    // control and the next click is treated as a focus
                    // recovery instead of a selection change. Same
                    // pattern as the DragRegion PointerReleased handler
                    // above, which had to bounce through the dispatcher
                    // for the same reason.
                    auto dq = self->DispatcherQueue();
                    if (!dq) return;
                    dq.TryEnqueue([weakSelf]() {
                        auto self2 = weakSelf.get();
                        if (!self2) return;
                        try {
                            if (auto* tab = self2->ActiveTab()) {
                                tab->Focus();
                            }
                        } catch (winrt::hresult_error const&) {}
                    });
                } catch (winrt::hresult_error const&) {
                }
            });

            InitGhostty();
            CreateTab();
        });
    }

    MainWindow::~MainWindow()
    {
        m_tabs.Clear();   // Tab destructors handle cleanup
        m_ghostty.reset(); // ghostty_app_free + config_free in correct order
        // Clean shutdown reached — clear the crash flag so the next launch
        // doesn't pause unnecessarily.
        std::error_code ec;
        std::filesystem::remove(crashFlagPath(), ec);
    }

    long __stdcall MainWindow::OnUnhandledException(struct _EXCEPTION_POINTERS* /*info*/) noexcept
    {
        // Best-effort cleanup before the OS kills the process. Each call here
        // is a Win32 / kernel API that's safe even with a corrupted heap;
        // ShowWindow / CloseHandle / MessageBoxW don't touch user-mode
        // structures that might be wrecked. If any of them does crash anyway,
        // the unhandled-exception filter "fails" recursively and WER takes
        // over with its standard dialog — same end result, just less polished.
        // That's an acceptable trade for keeping this code readable.
        OutputDebugStringA("GhosttyWin32: unhandled exception, attempting cleanup\n");
        if (g_mainWindow) {
            if (g_mainWindow->m_hwnd) ShowWindow(g_mainWindow->m_hwnd, SW_HIDE);
            for (auto& tab : g_mainWindow->m_tabs) {
                if (!tab) continue;
                if (auto* tc = tab->ActiveControl()) {
                    HANDLE h = tc->CompositionHandle();
                    if (h) CloseHandle(h);
                }
            }
        }
        MessageBoxW(nullptr,
            L"GhosttyWin32 hit a fatal error and must exit.\n\n"
            L"Restarting the app usually recovers.",
            L"GhosttyWin32",
            MB_OK | MB_ICONERROR | MB_TASKMODAL);
        // Don't swallow the exception — let WER / debugger see it as usual.
        return EXCEPTION_CONTINUE_SEARCH;
    }

    Tab* MainWindow::ActiveTab()
    {
        return m_tabs.Active(TabView());
    }

    TerminalControl* MainWindow::ActiveControl()
    {
        auto* tab = ActiveTab();
        return tab ? tab->ActiveControl() : nullptr;
    }

    void MainWindow::UpdateActivePanelVisibility()
    {
        // Walk AppContent.Children once: each child is a SplitPanel
        // for one Tab. The one matching the currently-active tab's
        // panel becomes Visible; everything else becomes Collapsed.
        // Collapsed elements stay parented (no Unloaded fires) so the
        // SwapChainPanel's DComp surface binding survives the switch.
        auto* tab = ActiveTab();
        winrt::GhosttyWin32::SplitPanel activePanel{ nullptr };
        if (tab) activePanel = tab->Panel();
        auto children = AppContent().Children();
        for (uint32_t i = 0; i < children.Size(); ++i) {
            auto child = children.GetAt(i).try_as<winrt::GhosttyWin32::SplitPanel>();
            if (!child) continue;
            bool isActive = activePanel && (child == activePanel);
            child.Visibility(isActive
                ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }
        // Pre-apply the focused visual on the newly-active leaf
        // synchronously, ahead of the dispatcher-deferred tab->Focus()
        // call in SelectionChanged. Without this, the panel goes
        // Visible while still in the "last LostFocus" state — its
        // UnfocusedDim overlay is shown for the one dispatcher tick
        // before XAML's GotFocus arrives and clears it — visible as a
        // brief darken/brighten flash on every tab switch. The actual
        // keyboard focus still goes through the deferred path (which
        // is what fires m_onFocused → NotifySurfaceFocused and updates
        // the active surface tracker); this only takes care of the
        // overlay's visibility so the visual state matches the
        // perceived selection immediately.
        if (tab) {
            if (auto* tc = tab->ActiveControl()) {
                tc->ApplyFocusVisual(true);
            }
        }
    }

    void MainWindow::UpdateDragRectangles()
    {
        // Convert DragRegion's current bounds to a single window-relative
        // physical-pixel RectInt32 and hand it to AppWindowTitleBar. Only
        // this rectangle responds to drag input; tabs and caption buttons
        // sit outside it so OS title-bar gestures (double-click maximize,
        // long-drag move) only fire from the strip's free space.
        //
        // Re-runs on DragRegion.SizeChanged: tab adds / removes change
        // the strip layout and so the DragRegion's bounds. DPI changes
        // also trigger a SizeChanged via the XAML relayout, so the
        // physical-pixel conversion stays current without a separate
        // DPI hook.
        if (!m_hwnd) return;
        auto dragRegion = DragRegion();
        if (!dragRegion) return;
        // Skip when the element hasn't been measured yet — TransformToVisual
        // works but ActualWidth/Height are 0, which would publish an
        // empty drag rect and let the OS treat the whole AppTitleBar as
        // non-drag content until the next size change. Bailing keeps
        // whatever rect was last published in effect.
        if (dragRegion.ActualWidth() <= 0 || dragRegion.ActualHeight() <= 0) return;

        auto content = Content();
        if (!content) return;
        auto transform = dragRegion.TransformToVisual(content);
        auto origin = transform.TransformPoint(winrt::Windows::Foundation::Point{0.0f, 0.0f});

        const double scale = static_cast<double>(GetDpiForWindow(m_hwnd)) / 96.0;
        winrt::Windows::Graphics::RectInt32 rect{
            static_cast<int32_t>(origin.X * scale),
            static_cast<int32_t>(origin.Y * scale),
            static_cast<int32_t>(dragRegion.ActualWidth() * scale),
            static_cast<int32_t>(dragRegion.ActualHeight() * scale)
        };
        try {
            AppWindow().TitleBar().SetDragRectangles({rect});
        } catch (winrt::hresult_error const&) {
            // AppWindow may not have a TitleBar yet during early init.
            // The next SizeChanged after the window is realised will
            // retry, so silently dropping the first attempt is fine.
        }
    }

    void MainWindow::RemoveTabPanelFromAppContent(Tab const& tab)
    {
        // Unparent the tab's SplitPanel from AppContent. Called by the
        // close paths before the Tab object is destroyed — ~Tab doesn't
        // know about AppContent (the factory deliberately keeps that
        // knowledge in the host), so without this the panel would leak
        // as an orphan child after Tab destruction.
        auto panel = tab.Panel();
        if (!panel) return;
        auto children = AppContent().Children();
        uint32_t idx = 0;
        if (children.IndexOf(panel, idx)) {
            children.RemoveAt(idx);
        }
    }

    void MainWindow::NotifySurfaceFocused(ghostty_surface_t surface) noexcept
    {
        m_activeSurface = surface;
    }

    // ----- IMainWindowView -----

    void MainWindow::Dispatch(std::function<void()> fn)
    {
        // Translate the winrt-free IMainWindowView::Dispatch seam
        // into a WinUI DispatcherQueue::TryEnqueue. Keeping winrt
        // confined to MainWindow (the only WinUI-aware piece) lets
        // GhosttyActions / GhosttyCallbackDispatcher / tests
        // depend on IMainWindowView without pulling in the
        // WindowsAppSDK projection headers.
        DispatcherQueue().TryEnqueue([fn = std::move(fn)]() { fn(); });
    }

    void MainWindow::Tick()
    {
        // Guard against the inert window state — Tick can fire from
        // a queued RENDER action after the window has begun tearing
        // down (m_ghostty already released). No-op in that case.
        if (m_ghostty) m_ghostty->Tick();
    }

    void MainWindow::RequestClose()
    {
        // WinUI's Window::Close throws when the window has already
        // begun tearing down; swallow so callers can fire and
        // forget. The hresult_error variant is the only one that
        // surfaces in practice (RPC_E_DISCONNECTED via the dispose
        // path).
        try { Close(); } catch (winrt::hresult_error const&) {}
    }

    void MainWindow::InitGhostty()
    {
        // Bring the dispatcher up before the runtime config so the
        // action_cb forwarder below can rely on it being ready by
        // the time ghostty fires its first action.
        m_ghosttyDispatcher = ghostty::CallbackDispatcher::Create(*this);

        ghostty_runtime_config_s rtConfig{};
        rtConfig.userdata = this;
        rtConfig.wakeup_cb = [](void*) {
            if (!g_mainWindow || !g_mainWindow->m_ghostty) return;
            g_mainWindow->DispatcherQueue().TryEnqueue([]() {
                if (g_mainWindow && g_mainWindow->m_ghostty) {
                    g_mainWindow->m_ghostty->Tick();
                }
            });
        };
        rtConfig.action_cb = [](ghostty_app_t, ghostty_target_s target, ghostty_action_s action) -> bool {
            // Thin forwarder. All dispatch + handler bodies live in
            // GhosttyCallbackDispatcher / GhosttyActions; the lambda
            // only exists because ghostty's runtime config wants a
            // C function pointer.
            if (!g_mainWindow || !g_mainWindow->m_ghosttyDispatcher) return false;
            return g_mainWindow->m_ghosttyDispatcher->DispatchAction(target, action);
        };
        rtConfig.read_clipboard_cb = [](void*, ghostty_clipboard_e, void* state) -> bool {
            if (!g_mainWindow) return false;
            auto* tc = g_mainWindow->ActiveControl();
            if (!tc || !tc->Surface()) return false;
            auto utf8 = interop::Encoding::toUtf8(win32::Clipboard::read(g_mainWindow->m_hwnd));
            if (utf8.empty()) return false;
            ghostty_surface_complete_clipboard_request(tc->Surface(), utf8.c_str(), state, false);
            return true;
        };
        rtConfig.confirm_read_clipboard_cb = [](void*, const char* content, void* state, ghostty_clipboard_request_e) {
            // Auto-confirm clipboard reads
            if (g_mainWindow) {
                auto* tc = g_mainWindow->ActiveControl();
                if (tc && tc->Surface()) {
                    ghostty_surface_complete_clipboard_request(tc->Surface(), content, state, true);
                }
            }
        };
        rtConfig.write_clipboard_cb = [](void*, ghostty_clipboard_e, const ghostty_clipboard_content_s* content, size_t count, bool) {
            if (!content || count == 0 || !content[0].data) return;
            HWND hwnd = g_mainWindow ? g_mainWindow->m_hwnd : nullptr;
            win32::Clipboard::write(hwnd, interop::Encoding::toUtf16(content[0].data));
        };
        // Shell exited (e.g. user typed `exit`), or ghostty asked to close
        // the surface for any other reason. The userdata is the PaneId
        // we set in TabFactory::MakeLeaf. Dispatch the UI mutation to
        // the next UI tick so it happens off the renderer thread.
        //
        // Two cases:
        //   * Leaf is the only pane in its tab → close the tab (same
        //     path as GHOSTTY_ACTION_CLOSE_TAB).
        //   * Leaf has a sibling → collapse the split. The surviving
        //     sibling takes the parent split's slot; if the closed
        //     pane was the active leaf, focus moves to the first leaf
        //     under the surviving subtree.
        rtConfig.close_surface_cb = [](void* userdata, bool /*process_alive*/) {
            if (!g_mainWindow || !userdata) return;
            PaneId id = PaneId::FromUserdata(userdata);
            auto mw = g_mainWindow;
            mw->DispatcherQueue().TryEnqueue([mw, id]() {
                mw->CloseSurfaceByPaneId(id);
            });
        };

        m_ghostty = ghostty::App::Create(rtConfig);
        if (m_ghostty && m_hwnd) {
            // Capture by raw `this`: MainWindow outlives every
            // TerminalControl it owns (the controls are destroyed
            // through Tabs, which is a MainWindow member), so the
            // lambda staying alive on the factory is safe.
            auto onLeafFocused = [this](ghostty_surface_t surface) noexcept {
                NotifySurfaceFocused(surface);
            };
            // GhosttyConfig wraps the raw ghostty_config_t with
            // typed, fallback-aware accessors so TabFactory (and
            // future callers) stop reimplementing the key-length /
            // fallback dance every time they need a config value.
            ghostty::Config cfg(m_ghostty->ConfigHandle());
            m_tabFactory = std::make_unique<TabFactory>(
                m_ghostty->Handle(),
                cfg,
                m_hwnd,
                m_paneIds,
                std::move(onLeafFocused));
        }
    }

    void MainWindow::CreateTab()
    {
        if (!m_ghostty || !m_hwnd) return;
        auto tv = TabView();

        auto item = muxc::TabViewItem();
        static constexpr wchar_t kDefaultTabTitle[] = L" ";
        item.Header(box_value(kDefaultTabTitle));
        item.IsClosable(true);
        // item.Content stays unset: the SplitPanel is parented under
        // AppContent (not under TabViewItem) so the tab strip can be
        // hidden independently of the terminal content. See the layout
        // comment in MainWindow.xaml. The append happens further down
        // after TabFactory::Make returns the panel.
        // Same focus-retention story as the AddTabButton: TabViewItem is
        // a Control with IsTabStop=true by default, so clicking a tab
        // header lands focus on the header itself rather than the inner
        // TerminalControl. Selection still works without IsTabStop —
        // it's driven by click, not keyboard tab order.
        item.IsTabStop(false);
        item.AllowFocusOnInteraction(false);
        tv.TabItems().Append(item);
        // Append-only — don't switch to the new tab yet. The SelectedItem
        // call (which is what makes the panel visible) is deferred to the
        // onActivated callback below, fired by Tab once ghostty has
        // presented its first frame to the swap chain. That way the panel
        // becomes visible only with real content — issue #22.

        // Tab activation work, fired on the UI thread via Tab once the
        // swap chain is bound to the panel and has at least one frame.
        auto weakThis = get_weak();
        auto itemStrong = item;
        auto tvStrong = tv;
        auto onActivated = [weakThis, itemStrong, tvStrong]() {
            auto self = weakThis.get();
            if (!self) return;
            // Setting SelectedItem realises the TerminalControl into
            // the visual tree, which fires its Loaded handler. Loaded
            // builds the per-control CoreTextEditContext (deferred
            // there because EditContext registration only takes hold
            // for an element that's actually in the live tree).
            tvStrong.SelectedItem(itemStrong);
            if (self->m_hwnd) ShowWindow(self->m_hwnd, SW_SHOW);
            // Focus is taken in the Activated event handler that fires
            // from the WM_ACTIVATE this ShowWindow posts. See the
            // comment on that handler for why anchoring focus there is
            // race-free, while a direct Focus() call here is not.
        };

        // Estimate the new panel's eventual size from the currently
        // active tab — the new panel will lay out into the same
        // AppContent row, so the active panel's size is the right
        // target. Passing this lets ghostty create the swap chain at
        // the right size from the start; without it the new panel's
        // ActualWidth is 0 (Collapsed until the deferred activation
        // fires) and ghostty falls back to the main window's full
        // client rect, which causes a "stretch then resize" flash as
        // soon as the panel becomes Visible.
        //
        // Values are PHYSICAL pixels — `cfg.initial_width` /
        // `initial_height` are the swap-chain buffer resolution. On
        // high-DPI (RDP 200 %, scaled displays) passing ActualWidth
        // straight through halves the swap-chain resolution, so the
        // new tab opens with the rendered content shrunk into the
        // corner of the panel until a window resize bumps it up to
        // the right size. Multiply by CompositionScale to match what
        // the SizeChanged handler does for every subsequent resize.
        //
        // First-tab case: ActiveControl() is null and AppContent has
        // already been measured (Activated fires after the first
        // layout pass), so AppContent.ActualWidth/Height (also DIPs
        // -> scaled) is the right fallback.
        uint32_t initialW = 0, initialH = 0;
        if (auto* prevControl = ActiveControl()) {
            auto prevPanel = prevControl->InnerPanel();
            double sx = prevPanel.CompositionScaleX();
            double sy = prevPanel.CompositionScaleY();
            if (sx <= 0.0) sx = 1.0;
            if (sy <= 0.0) sy = 1.0;
            initialW = static_cast<uint32_t>(prevPanel.ActualWidth()  * sx);
            initialH = static_cast<uint32_t>(prevPanel.ActualHeight() * sy);
        }
        if (initialW == 0 || initialH == 0) {
            auto content = AppContent();
            double sx = content.CompositionScaleX();
            double sy = content.CompositionScaleY();
            if (sx <= 0.0) sx = 1.0;
            if (sy <= 0.0) sy = 1.0;
            if (initialW == 0) initialW = static_cast<uint32_t>(content.ActualWidth()  * sx);
            if (initialH == 0) initialH = static_cast<uint32_t>(content.ActualHeight() * sy);
        }

        // Wrap TabFactory::Make in SEH guard so a hardware exception in
        // the NVIDIA driver during ghostty_surface_new (e.g.
        // dx_create_texture crash) doesn't kill the whole app and take
        // every other tab with it. The C++ work happens inside the
        // callback below.
        if (!m_tabFactory) return;
        struct CreateCtx {
            muxc::TabViewItem const* item;
            TabFactory* factory;
            std::function<void()> onActivated;
            uint32_t initialWidth;
            uint32_t initialHeight;
            std::unique_ptr<Tab> result;
        };
        CreateCtx ctx{ &item, m_tabFactory.get(), std::move(onActivated), initialW, initialH, nullptr };
        int ok = RunSEHGuarded([](void* arg) noexcept {
            auto* c = static_cast<CreateCtx*>(arg);
            c->result = c->factory->Make(*c->item, std::move(c->onActivated), c->initialWidth, c->initialHeight);
        }, &ctx);

        std::unique_ptr<Tab> tab = std::move(ctx.result);
        if (!ok) {
            // SEH caught a hardware exception inside TabFactory::Make — almost
            // always the NVIDIA driver memcpy crash. Process state is
            // unreliable from here (heap locks may be stuck, driver kernel
            // state corrupted, etc.) so don't try to continue.
            //
            // Hide the main window first — its XAML/D3D state may be
            // partially broken and showing it next to the message box
            // looks alarming. The dialog is parented to nullptr so it
            // stays visible after we hide the window.
            if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
            MessageBoxW(nullptr,
                L"A graphics driver error occurred while creating the new tab.\n"
                L"GhosttyWin32 will exit safely.\n\n"
                L"Restarting the app usually recovers — the next launch\n"
                L"will automatically wait 2 seconds for the driver.",
                L"GhosttyWin32",
                MB_OK | MB_ICONERROR | MB_TASKMODAL);
            if (m_hwnd) {
                PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            }
            return;
        }
        if (!tab) {
            // TabFactory::Make returned null cleanly (handle / attach / surface
            // creation failed but no hardware exception). Heap state is
            // intact, so just drop the orphan tab item and continue.
            auto items = tv.TabItems();
            uint32_t idx = 0;
            if (items.IndexOf(item, idx)) items.RemoveAt(idx);
            return;
        }

        // Parent the SplitPanel under AppContent (NOT TabViewItem) with
        // Visibility=Collapsed. The deferred onActivated callback flips
        // SelectedItem on the TabView, which fires SelectionChanged →
        // UpdateActivePanelVisibility, which is what actually makes
        // this panel Visible — so the "panel becomes visible only with
        // real content" invariant from issue #22 still holds. The panel
        // ref is captured by value (cheap winrt handle, just AddRef)
        // before the unique_ptr is moved into m_tabs so we don't reach
        // through a moved-from pointer.
        auto panel = tab->Panel();
        panel.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        AppContent().Children().Append(panel);
        // SelectedItem / SW_SHOW are deferred to the onActivated
        // callback fired from Tab once ghostty has presented its first
        // frame; focus + IME activation chain off SelectedItem via the
        // TerminalControl's Loaded → Focus → GotFocus path.
        m_tabs.Add(std::move(tab));
    }

    // ----- IMainWindowView: tab lifecycle / navigation / title -----
    // Callers (GhosttyActions) bounce through Dispatcher() before
    // entering these so the WinUI mutations land on the UI thread.

    void MainWindow::CloseTabBySurface(ghostty_surface_t surface)
    {
        // Mirror the TabCloseRequested handler — see there for why
        // Detach runs before RemoveAt and why the last tab's Tab
        // destruction is deferred to ~MainWindow.
        auto* t = m_tabs.FindBySurface(surface);
        if (!t) return;
        auto item = t->Item();
        // CLOSE_TAB closes the whole tab regardless of pane count —
        // detach every leaf so each swap chain handle is cleared
        // before unparent.
        t->DetachAll();
        auto tv = TabView();
        uint32_t idx = 0;
        if (tv.TabItems().IndexOf(item, idx)) {
            tv.TabItems().RemoveAt(idx);
        }
        DwmFlush();
        if (tv.TabItems().Size() == 0) {
            RequestClose();
        } else {
            // Mirror the TabCloseRequested handler: unparent the
            // SplitPanel from AppContent before destroying the Tab so
            // it doesn't leak as an orphan child.
            RemoveTabPanelFromAppContent(*t);
            m_tabs.Remove(item);
        }
    }

    void MainWindow::GoToTab(int requested)
    {
        auto tv = TabView();
        int count = static_cast<int>(tv.TabItems().Size());
        if (count == 0) return;
        int next = -1;
        switch (requested) {
            case GHOSTTY_GOTO_TAB_PREVIOUS: {
                int cur = tv.SelectedIndex();
                next = (cur - 1 + count) % count;
                break;
            }
            case GHOSTTY_GOTO_TAB_NEXT: {
                int cur = tv.SelectedIndex();
                next = (cur + 1) % count;
                break;
            }
            case GHOSTTY_GOTO_TAB_LAST:
                next = count - 1;
                break;
            default:
                if (requested >= 0 && requested < count) {
                    next = requested;
                }
                break;
        }
        if (next >= 0) tv.SelectedIndex(next);
    }

    void MainWindow::SetTabTitleForSurface(ghostty_surface_t surface, std::wstring title)
    {
        if (auto* t = m_tabs.FindBySurface(surface)) {
            t->Item().Header(box_value(winrt::hstring(title)));
        }
    }

    void MainWindow::CopyTabTitleForSurface(ghostty_surface_t surface)
    {
        auto* t = m_tabs.FindBySurface(surface);
        if (!t) return;
        auto title = winrt::unbox_value_or<winrt::hstring>(
            t->Item().Header(), winrt::hstring{});
        if (title.empty()) return;
        win32::Clipboard::write(m_hwnd, std::wstring(title));
    }

    void MainWindow::MoveActiveTabBy(ssize_t amount)
    {
        // Shift the currently-selected tab by `amount` positions
        // along the TabView's items collection. Clamped to the
        // bounds — out-of-range values are a no-op rather than a
        // wrap-around (matches the upstream MoveTab semantics).
        auto tabView = TabView();
        if (!tabView) return;
        auto items = tabView.TabItems();
        if (items.Size() == 0 || amount == 0) return;
        int current = tabView.SelectedIndex();
        if (current < 0) return;
        ssize_t target = static_cast<ssize_t>(current) + amount;
        ssize_t maxIdx = static_cast<ssize_t>(items.Size()) - 1;
        if (target < 0) target = 0;
        if (target > maxIdx) target = maxIdx;
        if (target == current) return;
        auto item = items.GetAt(static_cast<uint32_t>(current));
        items.RemoveAt(static_cast<uint32_t>(current));
        items.InsertAt(static_cast<uint32_t>(target), item);
        tabView.SelectedIndex(static_cast<int>(target));
    }

    // ----- IMainWindowView: state-owner delegating overrides -----

    void MainWindow::ApplySizeLimit(ghostty_action_size_limit_s limit)
    {
        m_sizeLimit.Apply(m_hwnd, limit);
    }

    void MainWindow::ToggleFullscreen()
    {
        m_fullscreen.Toggle(m_hwnd);
    }

    void MainWindow::PresentTerminal()
    {
        // Restore from minimized first so SetForegroundWindow has
        // something to focus. BringWindowToTop reorders the Z stack
        // even when foreground stealing is blocked by Win32's
        // SPI_GETFOREGROUNDLOCKTIMEOUT rule; the user notices the
        // taskbar flash even if focus is denied.
        if (!m_hwnd) return;
        if (IsIconic(m_hwnd)) {
            ShowWindow(m_hwnd, SW_RESTORE);
        }
        BringWindowToTop(m_hwnd);
        SetForegroundWindow(m_hwnd);
    }

    void MainWindow::ShowOnScreenKeyboard()
    {
        // osk.exe (the Accessibility on-screen keyboard) ships with
        // every supported Windows version and is the documented
        // entry point for keyboard-less input. TabTip.exe (the
        // touch keyboard) would be slightly nicer on tablets but
        // its path and launch contract changed across Win10
        // builds; osk.exe is the portable choice.
        ShellExecuteW(m_hwnd, L"open", L"osk.exe",
                      nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ----- IMainWindowView: terminal-driven appearance + lifecycle -----

    void MainWindow::ApplyBackgroundColor(uint8_t r, uint8_t g, uint8_t b)
    {
        if (m_hwnd) {
            COLORREF color = RGB(r, g, b);
            DwmSetWindowAttribute(m_hwnd, DWMWA_CAPTION_COLOR, &color, sizeof(color));
            float luminance = 0.299f * r + 0.587f * g + 0.114f * b;
            COLORREF textColor = (luminance < 128) ? RGB(255, 255, 255) : RGB(0, 0, 0);
            DwmSetWindowAttribute(m_hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));
        }
        auto brush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
            winrt::Windows::UI::Color{ 255, r, g, b });
        if (auto content = Content()) {
            content.as<winrt::Microsoft::UI::Xaml::Controls::Panel>().Background(brush);
        }
    }

    void MainWindow::SetCursorShapeForSurface(ghostty_surface_t surface,
                                              ghostty_action_mouse_shape_e shape)
    {
        // Route the shape to the leaf that actually owns `surface`, not
        // the tab's active leaf. MOUSE_SHAPE carries the originating
        // surface, and with split panes the pointer can be over a
        // non-active pane — using ActiveControl() landed the shape on
        // the wrong pane (#65). FindLeafBySurface walks the pane tree
        // and returns the owning leaf; in the single-pane case it
        // resolves to the same control ActiveControl() would.
        auto lookup = m_tabs.FindLeafBySurface(surface);
        if (!lookup.leaf) return;
        if (auto* tc = Tab::LeafToTerminalControl(*lookup.leaf)) {
            tc->SetCursorShape(shape);
        }
    }

    void MainWindow::ReplaceConfig(ghostty_config_t cloned)
    {
        if (!m_ghostty) {
            ghostty_config_free(cloned);
            return;
        }
        m_ghostty->ReplaceConfig(cloned);
    }

    void MainWindow::ReloadConfig(bool soft)
    {
        if (!m_ghostty) return;
        if (soft) {
            // Soft reload: re-apply the config we already hold.
            // Runs on whichever thread called us — m_ghostty's
            // handles are stable, and ghostty_app_update_config
            // is thread-safe on its own state.
            DispatcherQueue().TryEnqueue([this]() {
                if (!m_ghostty) return;
                auto app = m_ghostty->Handle();
                auto cfg = m_ghostty->ConfigHandle();
                if (app && cfg) ghostty_app_update_config(app, cfg);
            });
            return;
        }
        // Hard reload: ghostty's config parser stack-overflows the
        // default 1MB CreateThread stack on debug builds, so the
        // re-parse runs on a 4MB worker (same as ghostty::App::Create).
        // Result hand-off through the UI dispatcher matches the
        // CONFIG_CHANGE path.
        struct ReloadCtx { MainWindow* mw; };
        auto* ctx = new ReloadCtx{ this };
        HANDLE hThread = CreateThread(nullptr, 4 * 1024 * 1024,
            [](LPVOID p) -> DWORD {
                auto* c = static_cast<ReloadCtx*>(p);
                auto mwLocal = c->mw;
                delete c;
                ghostty_config_t newCfg = ghostty_config_new();
                if (newCfg) {
                    ghostty_config_load_default_files(newCfg);
                    ghostty_config_finalize(newCfg);
                }
                if (!mwLocal || !newCfg) {
                    if (newCfg) ghostty_config_free(newCfg);
                    return 0;
                }
                mwLocal->DispatcherQueue().TryEnqueue([mwLocal, newCfg]() {
                    if (!mwLocal->m_ghostty) {
                        ghostty_config_free(newCfg);
                        return;
                    }
                    ghostty_app_update_config(mwLocal->m_ghostty->Handle(), newCfg);
                    mwLocal->m_ghostty->ReplaceConfig(newCfg);
                });
                return 0;
            }, ctx, 0, nullptr);
        if (hThread) CloseHandle(hThread); else delete ctx;
    }

    void MainWindow::ShowDesktopNotification(ghostty_surface_t surface,
                                             std::wstring title, std::wstring body)
    {
        if (title.empty() && body.empty()) return;
        try {
            using namespace winrt::Microsoft::Windows::AppNotifications;
            using namespace winrt::Microsoft::Windows::AppNotifications::Builder;
            AppNotificationBuilder builder;
            if (!title.empty()) builder.AddText(title);
            if (!body.empty())  builder.AddText(body);
            // Embed the originating pane's id in the toast arguments
            // so a later click routes back to the right pane via
            // PresentNotification. Encoded as semicolon-separated
            // key=value pairs to leave room for future fields.
            if (auto lookup = m_tabs.FindLeafBySurface(surface);
                lookup.leaf && lookup.leaf->Id())
            {
                auto idStr = std::to_wstring(lookup.leaf->Id().value);
                builder.AddArgument(L"action", L"present");
                builder.AddArgument(L"surfaceId", winrt::hstring(idStr));
            }
            AppNotificationManager::Default().Show(builder.BuildNotification());
        } catch (winrt::hresult_error const&) {
            // Either the manager wasn't registered (App startup
            // logged it) or the OS refused. Nothing actionable
            // host-side; the message just doesn't appear.
        }
    }

    void MainWindow::PresentNotification(PaneId id)
    {
        // Step 1: retarget the active tab + leaf if we know which
        // pane the notification belonged to. A zero PaneId (launch
        // activation, missing argument, etc.) skips this and we just
        // foreground the window.
        if (id) {
            auto lookup = m_tabs.FindByPaneId(id);
            if (lookup.tab) {
                auto tabView = TabView();
                if (tabView) {
                    tabView.SelectedItem(lookup.tab->Item());
                }
                if (lookup.leaf) {
                    lookup.tab->SetActiveLeaf(lookup.leaf);
                }
                lookup.tab->Focus();
            }
        }
        // Step 2: bring the window to the foreground. PresentTerminal
        // handles the IsIconic / SW_RESTORE / SetForegroundWindow
        // dance; reusing it keeps the foreground-rule workaround in
        // one place.
        PresentTerminal();
    }

    void MainWindow::ReportProgress(ghostty_action_progress_report_s pr)
    {
        if (!m_hwnd) return;
        // ITaskbarList3 is cached on the UI thread (where COM is
        // STA-initialized) since CoCreateInstance + HrInit aren't
        // cheap to redo per OSC sequence.
        static winrt::com_ptr<ITaskbarList3> s_taskbar;
        if (!s_taskbar) {
            if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(s_taskbar.put())))) {
                return;
            }
            if (FAILED(s_taskbar->HrInit())) {
                s_taskbar = nullptr;
                return;
            }
        }
        TBPFLAG flag = TBPF_NOPROGRESS;
        switch (pr.state) {
            case GHOSTTY_PROGRESS_STATE_REMOVE:        flag = TBPF_NOPROGRESS;    break;
            case GHOSTTY_PROGRESS_STATE_SET:           flag = TBPF_NORMAL;        break;
            case GHOSTTY_PROGRESS_STATE_ERROR:         flag = TBPF_ERROR;         break;
            case GHOSTTY_PROGRESS_STATE_INDETERMINATE: flag = TBPF_INDETERMINATE; break;
            case GHOSTTY_PROGRESS_STATE_PAUSE:         flag = TBPF_PAUSED;        break;
        }
        s_taskbar->SetProgressState(m_hwnd, flag);
        // SetProgressValue is meaningless under INDETERMINATE /
        // NOPROGRESS and the percentage is -1 when no value was
        // reported — skip the call so the bar doesn't snap to 0%
        // on a bare state change.
        if (pr.progress >= 0
            && (flag == TBPF_NORMAL || flag == TBPF_ERROR || flag == TBPF_PAUSED)) {
            s_taskbar->SetProgressValue(m_hwnd,
                                        static_cast<ULONGLONG>(pr.progress),
                                        100ULL);
        }
    }

    namespace {
        // Depth-first search for the leaf hosting `surface`. The pane
        // tree is small (a handful of leaves at most) so a flat walk
        // is strictly cheaper than maintaining a side index.
        Pane* FindLeafForSurface(Pane* node, ghostty_surface_t surface) {
            if (!node) return nullptr;
            if (node->IsLeaf()) {
                auto* tc = Tab::LeafToTerminalControl(*node);
                return (tc && tc->Surface() == surface) ? node : nullptr;
            }
            if (auto* p = FindLeafForSurface(node->First(), surface)) return p;
            return FindLeafForSurface(node->Second(), surface);
        }

        // Returns the first leaf reached by depth-first descent — used
        // to pick a focus target after a split collapses and the
        // previously-active leaf is gone.
        Pane* FirstLeafIn(Pane* node) {
            if (!node) return nullptr;
            if (node->IsLeaf()) return node;
            if (auto* p = FirstLeafIn(node->First())) return p;
            return FirstLeafIn(node->Second());
        }

        // Push every leaf under `node` into `out` in depth-first
        // order — left subtree before right. PREVIOUS / NEXT pane
        // navigation iterates this list to find neighbours of the
        // currently active leaf.
        void CollectLeaves(Pane* node, std::vector<Pane*>& out) {
            if (!node) return;
            if (node->IsLeaf()) { out.push_back(node); return; }
            CollectLeaves(node->First(), out);
            CollectLeaves(node->Second(), out);
        }

        // Pick the leaf whose arranged rect is adjacent to `active`
        // in the requested cardinal direction. Filters to leaves
        // strictly on the requested side, then scores them by primary
        // distance (along the axis) plus a perpendicular penalty so
        // an aligned neighbour beats a far-off-axis one.
        //
        // Returns nullptr if no candidate qualifies — caller's job
        // to decide whether to fall back (today: just ignore the
        // input, matching how Windows Terminal handles "no neighbour
        // in this direction").
        Pane* FindAdjacentLeaf(Pane* active,
                               std::vector<Pane*> const& leaves,
                               ghostty_action_goto_split_e dir)
        {
            if (!active) return nullptr;
            auto a = active->ArrangedRect();
            float ax2 = a.X + a.Width;
            float ay2 = a.Y + a.Height;
            float aCenterX = a.X + a.Width  * 0.5f;
            float aCenterY = a.Y + a.Height * 0.5f;

            Pane* best = nullptr;
            double bestScore = std::numeric_limits<double>::max();
            for (auto* leaf : leaves) {
                if (leaf == active) continue;
                auto c = leaf->ArrangedRect();
                float cx2 = c.X + c.Width;
                float cy2 = c.Y + c.Height;
                float cCenterX = c.X + c.Width  * 0.5f;
                float cCenterY = c.Y + c.Height * 0.5f;

                double primary, perpendicular;
                bool valid = false;
                switch (dir) {
                case GHOSTTY_GOTO_SPLIT_LEFT:
                    // Candidate must end at or before active starts —
                    // allow a tiny overlap to absorb float rounding.
                    if (cx2 > a.X + 1.0f) break;
                    primary = a.X - cx2;
                    perpendicular = std::abs(cCenterY - aCenterY);
                    valid = true;
                    break;
                case GHOSTTY_GOTO_SPLIT_RIGHT:
                    if (c.X < ax2 - 1.0f) break;
                    primary = c.X - ax2;
                    perpendicular = std::abs(cCenterY - aCenterY);
                    valid = true;
                    break;
                case GHOSTTY_GOTO_SPLIT_UP:
                    if (cy2 > a.Y + 1.0f) break;
                    primary = a.Y - cy2;
                    perpendicular = std::abs(cCenterX - aCenterX);
                    valid = true;
                    break;
                case GHOSTTY_GOTO_SPLIT_DOWN:
                    if (c.Y < ay2 - 1.0f) break;
                    primary = c.Y - ay2;
                    perpendicular = std::abs(cCenterX - aCenterX);
                    valid = true;
                    break;
                default:
                    return nullptr;  // PREVIOUS / NEXT handled elsewhere
                }
                if (!valid) continue;
                // Weight perpendicular gap twice as heavily as primary
                // distance — keeps focus moves predictable when there
                // are off-axis panes that are technically closer in
                // straight-line distance.
                double score = primary + 2.0 * perpendicular;
                if (score < bestScore) {
                    bestScore = score;
                    best = leaf;
                }
            }
            return best;
        }
    }

    void MainWindow::SplitActivePane(ghostty_surface_t surface,
                                     ghostty_action_split_direction_e direction)
    {
        if (!m_tabFactory || !surface) return;
        auto* sourceTab = m_tabs.FindBySurface(surface);
        if (!sourceTab) return;
        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(sourceTab->Panel());
        if (!panelImpl) return;

        Pane* sourceLeaf = FindLeafForSurface(panelImpl->Root(), surface);
        if (!sourceLeaf || !sourceLeaf->IsLeaf()) return;

        // The source leaf's UIElement + PaneId are about to be moved
        // into a new wrapper leaf inside the split subtree we build
        // below. Capturing them here means the wrapper has its own
        // reference to the underlying TerminalControl before the
        // ReplaceLeaf call destroys the original Pane node.
        auto sourceContent = sourceLeaf->Content();
        PaneId sourcePaneId = sourceLeaf->Id();

        // ghostty's split-direction maps to (orientation, which-side-
        // does-the-new-pane-take). RIGHT/DOWN put the new pane after
        // the source on the layout axis; LEFT/UP put it before.
        SplitOrientation orient;
        bool newFirst;
        switch (direction) {
            case GHOSTTY_SPLIT_DIRECTION_RIGHT: orient = SplitOrientation::Horizontal; newFirst = false; break;
            case GHOSTTY_SPLIT_DIRECTION_LEFT:  orient = SplitOrientation::Horizontal; newFirst = true;  break;
            case GHOSTTY_SPLIT_DIRECTION_DOWN:  orient = SplitOrientation::Vertical;   newFirst = false; break;
            case GHOSTTY_SPLIT_DIRECTION_UP:    orient = SplitOrientation::Vertical;   newFirst = true;  break;
            default: return;
        }

        // Size hint for the new ghostty surface: the source pane's
        // current SwapChainPanel size halved on the split axis,
        // expressed in PHYSICAL pixels — `cfg.initial_width` /
        // `initial_height` are the swap-chain buffer resolution, not
        // a DIP measurement. On high-DPI (RDP 200 %, scaled monitors)
        // passing ActualWidth straight through halves the swap-chain
        // resolution, so the new pane renders into a buffer half the
        // pane's physical pixel footprint and the host's
        // SetMatrixTransform inverse-scale leaves the rendered content
        // shrunk into the corner with a black margin. Multiplying by
        // CompositionScale matches what the SizeChanged handler does
        // for every subsequent resize, so the swap chain is correctly-
        // sized from the first frame instead of waiting for a window
        // resize to bring it up to the right resolution.
        //
        // CompositionScale can transiently read 0 on a panel composition
        // hasn't picked up yet; fall back to 1.0 in that case (matches
        // the SizeChanged guard). The eventual CompositionScaleChanged
        // re-publishes the correct size via surface_set_size.
        uint32_t srcW = 0, srcH = 0;
        if (auto* srcTc = Tab::LeafToTerminalControl(*sourceLeaf)) {
            auto p = srcTc->InnerPanel();
            double scaleX = p.CompositionScaleX();
            double scaleY = p.CompositionScaleY();
            if (scaleX <= 0.0) scaleX = 1.0;
            if (scaleY <= 0.0) scaleY = 1.0;
            srcW = static_cast<uint32_t>(p.ActualWidth()  * scaleX);
            srcH = static_cast<uint32_t>(p.ActualHeight() * scaleY);
        }
        uint32_t newW = (orient == SplitOrientation::Horizontal) ? srcW / 2 : srcW;
        uint32_t newH = (orient == SplitOrientation::Vertical)   ? srcH / 2 : srcH;

        // Wrap MakeLeaf in an SEH guard for the same reason CreateTab
        // does — ghostty_surface_new calls into dx_create_texture
        // where NVIDIA drivers have historically thrown hardware
        // exceptions. Without the guard, a driver AV here takes down
        // every other pane / tab in the window. With it, we can fail
        // closed for the new pane while leaving the rest of the
        // window intact (or, if the heap is clearly corrupt, exit
        // cleanly via the same cleanup path).
        struct SplitCtx {
            TabFactory* factory;
            uint32_t initialWidth;
            uint32_t initialHeight;
            std::unique_ptr<Pane> result;
        };
        SplitCtx ctx{ m_tabFactory.get(), newW, newH, nullptr };
        int ok = RunSEHGuarded([](void* arg) noexcept {
            auto* c = static_cast<SplitCtx*>(arg);
            c->result = c->factory->MakeLeaf(c->initialWidth, c->initialHeight);
        }, &ctx);
        if (!ok) {
            // Driver-side hardware exception. Process state is
            // unreliable from here — same recovery path as the tab-
            // creation crash: hide window, show explanatory dialog,
            // post WM_CLOSE.
            if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
            MessageBoxW(nullptr,
                L"A graphics driver error occurred while creating the new split.\n"
                L"GhosttyWin32 will exit safely.\n\n"
                L"Restarting the app usually recovers — the next launch\n"
                L"will automatically wait 2 seconds for the driver.",
                L"GhosttyWin32",
                MB_OK | MB_ICONERROR | MB_TASKMODAL);
            if (m_hwnd) PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            return;
        }
        auto newLeaf = std::move(ctx.result);
        if (!newLeaf) return;
        Pane* newLeafPtr = newLeaf.get();
        auto newControl = newLeaf->Content().try_as<winrt::GhosttyWin32::TerminalControl>();

        // Build the replacement subtree: a split node whose children
        // are (a) a wrapper around the original source content and
        // (b) the new leaf, ordered per `newFirst`.
        auto sourceWrapper = Pane::MakeLeaf(sourceContent, sourcePaneId);
        auto subtree = newFirst
            ? Pane::MakeSplit(orient, 0.5, std::move(newLeaf), std::move(sourceWrapper))
            : Pane::MakeSplit(orient, 0.5, std::move(sourceWrapper), std::move(newLeaf));

        if (!panelImpl->ReplaceLeaf(sourceLeaf, std::move(subtree))) {
            // Tree mutation failed after the new surface was already
            // attached — detach so it doesn't leak.
            if (newControl) {
                if (auto* tc = winrt::get_self<implementation::TerminalControl>(newControl)) {
                    tc->Detach();
                }
            }
            return;
        }

        // Focus shifts to the freshly-created pane: matches the
        // expectation set by every other terminal (a `:vsplit` lands
        // the cursor in the new pane).
        sourceTab->SetActiveLeaf(newLeafPtr);
        if (newControl) {
            newControl.Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
        }
    }

    void MainWindow::EqualizeSplitsForSurface(ghostty_surface_t surface)
    {
        if (!surface) return;
        auto* tab = m_tabs.FindBySurface(surface);
        if (!tab) return;
        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(tab->Panel());
        if (!panelImpl) return;
        panelImpl->EqualizeAll();
    }

    void MainWindow::ToggleSplitZoomForSurface(ghostty_surface_t surface)
    {
        if (!surface) return;
        auto* tab = m_tabs.FindBySurface(surface);
        if (!tab) return;
        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(tab->Panel());
        if (!panelImpl) return;

        // Already zoomed → unzoom regardless of which pane fired the
        // action. Matches how Windows Terminal / iTerm exit zoom mode:
        // a second press anywhere collapses it back.
        if (panelImpl->ZoomedLeaf()) {
            panelImpl->SetZoomed(nullptr);
            return;
        }

        Pane* leaf = FindLeafForSurface(panelImpl->Root(), surface);
        if (!leaf) return;
        // Single-leaf tabs skip the zoom — there's nothing to expand
        // against, and the visual state would be identical to the
        // normal layout.
        if (leaf == panelImpl->Root()) return;

        panelImpl->SetZoomed(leaf);
        tab->SetActiveLeaf(leaf);
        // Re-focus so the zoomed pane keeps input even when zoom was
        // toggled from a non-active pane via a remapped binding.
        if (auto control = leaf->Content().try_as<winrt::GhosttyWin32::TerminalControl>()) {
            control.Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
        }
    }

    void MainWindow::GotoSplitFromAction(ghostty_surface_t surface,
                                         ghostty_action_goto_split_e direction)
    {
        if (!surface) return;
        auto* tab = m_tabs.FindBySurface(surface);
        if (!tab) return;
        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(tab->Panel());
        if (!panelImpl) return;

        Pane* active = FindLeafForSurface(panelImpl->Root(), surface);
        if (!active) return;

        std::vector<Pane*> leaves;
        CollectLeaves(panelImpl->Root(), leaves);
        if (leaves.size() <= 1) return;  // nothing to navigate to

        Pane* target = nullptr;
        if (direction == GHOSTTY_GOTO_SPLIT_PREVIOUS
            || direction == GHOSTTY_GOTO_SPLIT_NEXT) {
            // Cycle through DFS order. wrap-around so the last pane's
            // NEXT lands on the first and vice versa.
            auto it = std::find(leaves.begin(), leaves.end(), active);
            if (it == leaves.end()) return;
            size_t idx = static_cast<size_t>(std::distance(leaves.begin(), it));
            size_t newIdx;
            if (direction == GHOSTTY_GOTO_SPLIT_NEXT) {
                newIdx = (idx + 1) % leaves.size();
            } else {
                newIdx = (idx == 0) ? leaves.size() - 1 : idx - 1;
            }
            target = leaves[newIdx];
        } else {
            target = FindAdjacentLeaf(active, leaves, direction);
        }
        if (!target || target == active) return;

        tab->SetActiveLeaf(target);
        if (auto element = target->Content()) {
            if (auto control = element.try_as<winrt::GhosttyWin32::TerminalControl>()) {
                control.Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
            }
        }
    }

    void MainWindow::ResizeSplitFromAction(ghostty_surface_t surface,
                                           ghostty_action_resize_split_s resize)
    {
        if (!surface) return;
        auto* tab = m_tabs.FindBySurface(surface);
        if (!tab) return;
        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(tab->Panel());
        if (!panelImpl) return;

        Pane* leaf = FindLeafForSurface(panelImpl->Root(), surface);
        if (!leaf) return;

        // The split axis we're resizing matches the direction axis:
        // LEFT/RIGHT → Horizontal split, UP/DOWN → Vertical split.
        SplitOrientation needOrient =
            (resize.direction == GHOSTTY_RESIZE_SPLIT_LEFT
             || resize.direction == GHOSTTY_RESIZE_SPLIT_RIGHT)
            ? SplitOrientation::Horizontal
            : SplitOrientation::Vertical;

        // Walk up to the nearest ancestor split with the right axis.
        // `child` is the descendant of that ancestor that contains the
        // active leaf — used to figure out whether the leaf is on the
        // first/second side of the split so the direction sign is
        // applied correctly.
        Pane* node = leaf;
        Pane* child = nullptr;
        while (node && node->Parent()) {
            auto* parent = node->Parent();
            if (parent->Orientation() == needOrient) {
                child = node;
                node = parent;
                break;
            }
            node = parent;
        }
        if (!child || !node || node->IsLeaf()) return;

        auto rect = node->ArrangedRect();
        float extent = (needOrient == SplitOrientation::Horizontal) ? rect.Width : rect.Height;
        float useable = std::max(1.0f,
            extent - static_cast<float>(implementation::SplitPanel::kSplitterThickness));
        double deltaRatio = static_cast<double>(resize.amount) / useable;

        // Arrow direction == direction the boundary moves, regardless
        // of which side of the split the active pane is on.
        //   * RIGHT / DOWN move the boundary toward +axis → ratio
        //     grows (first child gets larger).
        //   * LEFT / UP move the boundary toward -axis → ratio shrinks.
        // The previous "flip the sign when the active pane is the
        // second child" logic was a tmux-style "grow the active pane
        // in the arrow direction" rule that surprised the user when
        // pressing LEFT from the right-hand pane moved the boundary
        // right instead of left.
        bool increase = (resize.direction == GHOSTTY_RESIZE_SPLIT_RIGHT
                      || resize.direction == GHOSTTY_RESIZE_SPLIT_DOWN);

        node->SetRatio(node->Ratio() + (increase ? deltaRatio : -deltaRatio));
        panelImpl->InvalidateMeasure();
        panelImpl->InvalidateArrange();
    }

    void MainWindow::CloseSurfaceByPaneId(PaneId id)
    {
        auto lookup = m_tabs.FindByPaneId(id);
        if (!lookup.tab || !lookup.leaf) return;
        auto* tab = lookup.tab;
        auto* leaf = lookup.leaf;

        // Detach first so the surface / DComp handle are released
        // synchronously, before the Pane node holding the
        // TerminalControl is destroyed.
        if (auto* tc = Tab::LeafToTerminalControl(*leaf)) {
            // Clear m_activeSurface if it pointed at the surface we're
            // about to free — the focused-surface cache must never
            // outlive the underlying ghostty_surface_t. The next
            // TerminalControl::GotFocus on the retargeted sibling (or
            // a new tab) will refill the slot.
            if (tc->Surface() == m_activeSurface) m_activeSurface = nullptr;
            tc->Detach();
        }

        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(tab->Panel());
        if (!panelImpl) return;

        // Identify the sibling subtree BEFORE the removal so we can
        // retarget the active leaf into it (the leaf pointer is about
        // to be invalidated).
        Pane* sibling = nullptr;
        bool closingActive = (tab->ActiveLeaf() == leaf);
        if (auto* parent = leaf->Parent()) {
            sibling = (parent->First() == leaf) ? parent->Second() : parent->First();
        }
        // Clear the active-leaf pointer up front: regardless of which
        // branch runs below, leaving it pointing at the doomed leaf
        // would dangle until the SetActiveLeaf calls overwrite it.
        if (closingActive) tab->SetActiveLeaf(nullptr);

        auto result = panelImpl->RemoveLeaf(leaf);
        if (result == implementation::SplitPanel::RemovalResult::Collapsed) {
            // Tab survives; retarget focus to the surviving subtree.
            if (closingActive && sibling) {
                if (auto* newActive = FirstLeafIn(sibling)) {
                    tab->SetActiveLeaf(newActive);
                    if (auto* tc = Tab::LeafToTerminalControl(*newActive)) {
                        auto element = newActive->Content();
                        if (auto control = element.try_as<winrt::GhosttyWin32::TerminalControl>()) {
                            control.Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
                        }
                    }
                }
            }
            return;
        }

        // RemovedRoot or NotFound — treat as full-tab close.
        // (NotFound shouldn't happen, but failing closed by closing
        // the tab is the safer recovery than leaving a half-detached
        // pane around.) DetachAll is idempotent against the leaf we
        // already detached above and sweeps any remaining ones.
        tab->DetachAll();
        auto item = tab->Item();
        auto tv = TabView();
        uint32_t idx = 0;
        if (tv.TabItems().IndexOf(item, idx)) {
            tv.TabItems().RemoveAt(idx);
        }
        DwmFlush();
        if (tv.TabItems().Size() == 0) {
            Close();
        } else {
            m_tabs.Remove(item);
        }
    }

    // Caption button click handlers. We route through Win32 messages
    // rather than OverlappedPresenter state changes (which tripped the
    // NVIDIA driver crash in issue #26). The OS handles min/max/restore
    // through its standard NCA path and we just observe the result via
    // AppWindow.Changed → UpdateMaximizeGlyph.

    void MainWindow::OnMinimizeClick(winrt::Windows::Foundation::IInspectable const&,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_hwnd) ShowWindow(m_hwnd, SW_MINIMIZE);
    }

    void MainWindow::OnMaximizeClick(winrt::Windows::Foundation::IInspectable const&,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_hwnd) return;
        SendMessageW(m_hwnd, WM_SYSCOMMAND,
                     IsZoomed(m_hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
    }

    void MainWindow::OnCloseClick(winrt::Windows::Foundation::IInspectable const&,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        // Route through WM_CLOSE so any registered close hooks run; the
        // window's own Close() shortcut would skip them.
        if (m_hwnd) PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        else this->Close();
    }

    void MainWindow::UpdateMaximizeGlyph()
    {
        if (!m_hwnd) return;
        // E922 = ChromeMaximize (□), E923 = ChromeRestore (❐).
        wchar_t const* glyph = IsZoomed(m_hwnd) ? L"\xE923" : L"\xE922";
        try {
            MaximizeGlyph().Glyph(glyph);
        } catch (winrt::hresult_error const&) {
            // XAML may not have finished loading the named element yet;
            // the next Changed/SizeChanged tick will retry.
        }
    }

}
