#include "pch.h"
#include "MainWindow.xaml.h"
#include "App.xaml.h"
#include "Ghostty/CallbackDispatcher.h"
#include "Host/KeyModifiers.h"
#include "Interop/Encoding.h"
#include "Display/PhysicalPixels.h"
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
#include <cctype>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace muxc = Microsoft::UI::Xaml::Controls;

namespace winrt::GhosttyWin32::implementation
{
    MainWindow::MainWindow()
    {
        // Adopt the App-scope ghostty wrapper as a class invariant.
        // App::OnLaunched created ghostty before make<MainWindow>()
        // and aborted on failure, so by the time a MainWindow exists
        // the borrow below is guaranteed non-null; every method on
        // this class can read `m_ghosttyApp->Foo()` without
        // re-checking. The pointer's lifetime is safe by App's
        // destructor order — App destroys its `window` member
        // (containing this MainWindow) BEFORE `m_ghostty`, so the
        // borrow can't outlive its target.
        m_ghosttyApp = App::g_app->Ghostty();

        // Same lifetime story as m_ghosttyApp: this MainWindow exists
        // for the dispatcher's entire life, and `*this` is already a
        // valid C++ object inside the ctor body. Building the
        // dispatcher here (rather than waiting for Activated) keeps
        // the action_cb forwarder safe even if it fires through any
        // pre-Activated edge case.
        // The AppHooks slots are App-scope: "add another top-level
        // window" and friends don't belong on IWindow, so the factory
        // takes them as callables the App fills in. Same shape as
        // MainWindowRuntime's Host bundle for other cross-scope hooks.
        m_ghosttyDispatcher = ghostty::CallbackDispatcher::Create(
            *this,
            {
                .newWindow = []() {
                    if (App::g_app) App::g_app->CreateNewWindow();
                },
                .closeAllWindows = []() {
                    if (App::g_app) App::g_app->CloseAllWindows();
                },
                .quit = []() {
                    if (App::g_app) App::g_app->Quit();
                },
                .gotoWindow = [](ghostty_action_goto_window_e d) {
                    if (App::g_app) App::g_app->PresentWindow(d);
                },
            });

        ExtendsContentIntoTitleBar(true);

        Activated([this](auto&&, auto&&) {
            if (m_activatedOnce) return;
            m_activatedOnce = true;

            // Enter the App-scope aggregate. Every caller —
            // runtime callbacks, target-based routing, the SEH
            // handler — consults this collection to reach a live
            // MainWindow; no file-scope static shortcut remains.
            if (App::g_app) App::g_app->Windows().Register(this);
            auto windowNative = this->try_as<::IWindowNative>();
            if (windowNative) windowNative->get_WindowHandle(&m_hwnd);
            // The pre-first-frame hide avoids flashing an empty window
            // before ghostty presents. A drop host receives a tab that
            // is already presenting, so it has frames to show from the
            // first paint and skips the hide.
            if (m_hwnd && !m_suppressInitialTab) ShowWindow(m_hwnd, SW_HIDE);

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
                            // Window-level activation crosses windows
                            // without firing the control's LostFocus
                            // (XAML logical focus stays on the control
                            // while the window is inactive), so drop
                            // the renderer-side focus here. Gated on
                            // the OS foreground truth + deduped:
                            // WinUI3 Activated oscillates with
                            // multiple windows on one thread, and
                            // forwarding it verbatim spams the
                            // renderer with .focus messages — each of
                            // which produces a frame, which is enough
                            // continuous presenting to re-trigger the
                            // multi-window overlap flicker.
                            if (self->m_rendererFocus &&
                                !self->IsForeground()) {
                                self->m_rendererFocus = false;
                                tc->Surface().SetFocus(false);
                            }
                        }
                        // Title-bar HTCAPTION modal-loop recovery is
                        // handled by the DragRegion PointerReleased
                        // handler set up further down — it re-focuses
                        // through the dispatcher regardless of how
                        // Activated resolves the click, which is the
                        // reliable path. The earlier
                        // GetForegroundWindow-based re-Activate here
                        // was a supplementary recovery for the same
                        // scenario; with multiple top-level windows
                        // it fires spuriously on legitimate cross-
                        // window switches — the check briefly reads
                        // our HWND while Windows is still routing the
                        // WM_ACTIVATE pair between siblings — and
                        // ping-pongs focus back to whichever window
                        // deactivated last. Trust WinUI's Deactivated
                        // as authoritative and let PointerReleased
                        // handle the drag case.
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
                                // Bail unless this window is REALLY the
                                // OS foreground by the time this runs.
                                // With two windows on one thread the
                                // Activated events oscillate, and
                                // running this restore path on a
                                // window that has already lost
                                // activation is what sustains the
                                // ping-pong: Focus() on an element in
                                // an inactive window re-activates that
                                // window, yanking foreground back from
                                // the sibling — whose own pending
                                // restore then yanks it again, forever.
                                // The OS foreground check at execution
                                // (not enqueue) time breaks the cycle:
                                // a stale restore becomes a no-op.
                                if (!self->IsForeground()) {
                                    return;
                                }
                                if (auto* tab = self->ActiveTab()) {
                                    tab->Focus();
                                }
                                if (auto* tc = self->ActiveControl()) {
                                    tc->NotifyImeFocusEnter();
                                    // Counterpart of the Deactivated
                                    // branch: if XAML focus never left
                                    // the control, GotFocus won't
                                    // re-fire, so restore the renderer
                                    // focus explicitly (deduped).
                                    if (!self->m_rendererFocus) {
                                        self->m_rendererFocus = true;
                                        tc->Surface().SetFocus(true);
                                    }
                                }
                            } catch (winrt::hresult_error const&) {
                            }
                        });
                } catch (winrt::hresult_error const&) {
                }
            });

            // Renderer-side occlusion. While the window is hidden
            // (minimize, Win+D, tray) every surface's renderer thread
            // can stop producing frames entirely instead of ticking
            // blink / safety-net presents; VisibilityChanged fires for
            // both directions and the restore path redraws once from
            // the renderer's .visible handler, so no stale frame is
            // shown. Same weak_ref/try-catch rationale as the
            // Activated handler above.
            VisibilityChanged([weakActivated](
                winrt::Windows::Foundation::IInspectable const&,
                winrt::Microsoft::UI::Xaml::WindowVisibilityChangedEventArgs const& args) {
                auto self = weakActivated.get();
                if (!self) return;
                try {
                    self->BroadcastOcclusion(args.Visible());
                } catch (winrt::hresult_error const&) {
                }
            });

            auto tv = TabView();

            // ----- tab drag-out / merge (release-time semantics) -----
            // Tab movement uses TabView's classic drag-and-drop flow
            // (CanDragTabs, set in markup) rather than the WinAppSDK
            // native tear-out (CanTearOutTabs). Native tear-out drives
            // an OS move-size loop that spawns and drags a live window
            // mid-drag; in practice it shipped broken
            // (microsoft-ui-xaml#10154 raises the window request for
            // plain clicks, #10156 zeroes the NewWindowId round-trip)
            // and mispositions the grab point when the torn-out window
            // appears. With drag-and-drop nothing happens until the
            // user RELEASES: dropping on another window's strip merges
            // the tab there, dropping anywhere else spawns a window at
            // the drop point. Only a drag ghost is shown mid-drag.
            //
            // The dragged TabViewItem travels through App's
            // dragged-tab slot (SetDraggedTab), NOT the DataPackage:
            // a drop on another top-level window rides OLE, which
            // only marshals primitive package values — a TabViewItem
            // stuffed into Properties comes out missing on the other
            // window and the merge silently degrades into a
            // drop-outside. The package deliberately stays empty
            // (operation only) so foreign drop targets — a text
            // editor, say — have nothing to accept and can't swallow
            // the drag away from TabDroppedOutside.
            tv.TabDragStarting([](auto const&, auto const& args) {
                if (App::g_app) App::g_app->SetDraggedTab(args.Tab());
                args.Data().RequestedOperation(
                    winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
            });

            // Fires on the source when the drag ends, whether or not
            // any drop landed — the one reliable place to clear the
            // in-flight slot.
            tv.TabDragCompleted([](auto const&, auto const&) {
                if (App::g_app) App::g_app->ClearDraggedTab();
            });

            tv.TabStripDragOver([](auto const&, auto const& e) {
                if (App::g_app && App::g_app->DraggedTab()) {
                    e.AcceptedOperation(
                        winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
                }
            });

            tv.TabStripDrop([weakActivated](auto const& sender, auto const& e) {
                auto self = weakActivated.get();
                if (!self || !App::g_app) return;
                // TabStripDrop is a plain DragEventHandler, so the
                // sender arrives as IInspectable, not a typed TabView.
                auto strip = sender.template try_as<muxc::TabView>();
                if (!strip) return;
                // Source's TabDragCompleted (which clears the slot)
                // fires only after this drop returns.
                auto item = App::g_app->DraggedTab();
                if (!item) return;
                auto* source = App::g_app->FindWindowByTabItem(item);
                // Same-strip drops are reorders, which CanReorderTabs
                // already handled natively.
                if (!source || source == self.get()) return;
                // Insert where the tab was dropped: before the first
                // tab whose slot the pointer hasn't fully passed.
                int32_t index = -1;
                auto items = strip.TabItems();
                for (uint32_t i = 0; i < items.Size(); ++i) {
                    auto container = strip.ContainerFromIndex(i)
                        .template try_as<muxc::TabViewItem>();
                    if (!container) continue;
                    if (e.GetPosition(container).X - container.ActualWidth() < 0) {
                        index = static_cast<int32_t>(i);
                        break;
                    }
                }
                try {
                    if (auto tab = source->ReleaseTornOutTab(item)) {
                        self->AdoptTornOutTab(std::move(tab), index);
                        source->CloseIfTornOutEmpty();
                    }
                } catch (winrt::hresult_error const&) {
                    // A failed move must not take either window down;
                    // whichever side holds the unique_ptr owns the tab.
                }
            });

            tv.TabDroppedOutside([weakActivated](auto const&, auto const& args) {
                auto self = weakActivated.get();
                if (!self || !App::g_app) return;
                auto item = args.Tab();
                if (!item) return;
                POINT cursor{};
                bool haveCursor = GetCursorPos(&cursor) != 0;
                // Offsets place the window so its tab strip lands near
                // the pointer instead of the window's top-left corner.
                int32_t dropX = static_cast<int32_t>(cursor.x) - 120;
                int32_t dropY = static_cast<int32_t>(cursor.y) - 24;
                // Dragging out the only tab must not leave an empty
                // shell behind — just move this window to the drop
                // point instead, browser-style.
                if (self->m_tabs.Size() <= 1) {
                    if (haveCursor) {
                        self->AppWindow().Move({ dropX, dropY });
                    }
                    return;
                }
                auto* host = App::g_app->CreateTearOutWindow();
                if (!host) return;
                try {
                    if (auto tab = self->ReleaseTornOutTab(item)) {
                        host->AdoptTornOutTab(std::move(tab), -1);
                        if (haveCursor) {
                            host->AppWindow().Move({ dropX, dropY });
                        }
                        // The drag is over, so activation is safe —
                        // hand the new window focus like a browser
                        // does after a tab is torn off.
                        host->Activate();
                    } else {
                        // Nothing moved; don't leak an empty host.
                        host->RequestClose();
                    }
                } catch (winrt::hresult_error const&) {
                }
            });

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
            // Subscribe to system theme changes and push the current
            // OS light/dark preference straight through to ghostty so
            // themes with light/dark variants pick the right one at
            // launch without waiting for the first user toggle.
            HookSystemThemeSignal();
            PushCurrentSystemColorScheme();
            // Tear-out hosts don't create an initial tab: they adopt
            // the dragged one (possibly before this handler even ran).
            if (!m_suppressInitialTab) CreateTab();
            // Start the 1 Hz foreground-pid poll now that ghostty is
            // up and a tab exists (or, for tear-out hosts, is about to
            // be adopted). Idempotent — the timer only starts once.
            StartForegroundPidPoll();
            // Honour `window-decoration` from config at startup so
            // users who set it in their config see the chrome state
            // they asked for without having to fire the toggle
            // keybind first. After this, TOGGLE flips per-window
            // overrides on top of the config baseline.
            //
            // Order: must run AFTER CreateTab. The first tab's swap
            // chain takes its initial size hint from AppContent's
            // measured bounds; collapsing AppTitleBar before the first
            // measure leaves AppContent unresolved (no layout pass has
            // run between Apply and CreateTab inside this synchronous
            // activation lambda), so the very first surface starts at
            // 0×0 and ghostty's renderer never publishes a frame.
            // Running Apply after CreateTab lets the tab come up at
            // the chrome-visible size, then the relayout shrinks
            // AppTitleBar and AppContent expands — SizeChanged fires
            // and ghostty resizes naturally.
            ApplyWindowDecorationsAppearance();
        });
    }

    MainWindow::~MainWindow()
    {
        // Stop the foreground-pid poll before anything else in this
        // destructor runs — its Tick callback captures a weak_ref
        // that would return null at this point anyway, but Stopping
        // explicitly avoids one final call sneaking through XAML's
        // dispatch queue during teardown.
        if (m_foregroundPidTimer) {
            try { m_foregroundPidTimer.Stop(); }
            catch (winrt::hresult_error const&) {}
        }
        // Take ourselves off the App-scope aggregate before anything
        // else — subsequent runtime callbacks that fire during
        // ghostty_app_free's join land in the FindForSurface / Any
        // paths and must not find a half-torn-down window.
        if (App::g_app) App::g_app->Windows().Unregister(this);
        m_tabs.Clear();   // Tab destructors handle cleanup
        // ghostty::App ownership lives on App scope now (#55 prep).
        // App's destructor frees ghostty AFTER its `window` member
        // (this MainWindow) has gone, so the surface/IO-thread join
        // inside ghostty_app_free finds nothing left to wait on. The
        // crash flag is also App's concern: App::~App clears it once
        // per clean process shutdown, which is the granularity the
        // "did the previous run crash?" check actually wants.
    }

    namespace {
        // Subclass ID for the WM_SETTINGCHANGE hook. Distinct from
        // SizeLimit's (which uses id=1) so both subclasses coexist
        // on the same HWND without either replacing the other.
        constexpr UINT_PTR kSystemThemeSubclassId = 2;

        // Read HKCU\...\Themes\Personalize\AppsUseLightTheme. Missing
        // key or read failure defaults to DARK — the terminal's
        // typical setting and the safer choice for a code-signed app
        // running before the personalization service is ready.
        ghostty_color_scheme_e ReadOsColorScheme() noexcept {
            DWORD value = 0;
            DWORD size = sizeof(value);
            LSTATUS s = RegGetValueW(
                HKEY_CURRENT_USER,
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme",
                RRF_RT_REG_DWORD,
                nullptr, &value, &size);
            if (s != ERROR_SUCCESS) return GHOSTTY_COLOR_SCHEME_DARK;
            return value ? GHOSTTY_COLOR_SCHEME_LIGHT : GHOSTTY_COLOR_SCHEME_DARK;
        }

        LRESULT CALLBACK SystemThemeSubclassProc(
            HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
            UINT_PTR /*id*/, DWORD_PTR ref) noexcept
        {
            if (msg == WM_SETTINGCHANGE && lp) {
                // lParam is a wchar_t* naming the changed setting. The
                // one we care about is "ImmersiveColorSet" (fires when
                // the OS light/dark preference flips). Compare via
                // wcscmp; other WM_SETTINGCHANGE payloads (font sizes,
                // input languages) sail past unchanged.
                auto* name = reinterpret_cast<const wchar_t*>(lp);
                if (wcscmp(name, L"ImmersiveColorSet") == 0) {
                    if (auto* self = reinterpret_cast<MainWindow*>(ref)) {
                        self->PushCurrentSystemColorScheme();
                    }
                }
            }
            return DefSubclassProc(hwnd, msg, wp, lp);
        }
    }

    void MainWindow::HookSystemThemeSignal() noexcept
    {
        if (!m_hwnd) return;
        SetWindowSubclass(m_hwnd, &SystemThemeSubclassProc,
                          kSystemThemeSubclassId,
                          reinterpret_cast<DWORD_PTR>(this));
    }

    void MainWindow::PushCurrentSystemColorScheme() noexcept
    {
        auto scheme = ReadOsColorScheme();
        if (m_ghosttyApp) m_ghosttyApp->SetColorScheme(scheme);
        // ghostty_app_set_color_scheme updates only the app-level
        // conditional state and triggers a soft reload, but each
        // surface derives config against its OWN conditional state —
        // which stays on the pre-flip theme, so the reload picks the
        // old variant and the surface's palette doesn't change until
        // it's recreated. Push the new scheme into each existing
        // surface too so the flip lands immediately.
        for (auto& tab : m_tabs) {
            if (!tab) continue;
            auto* panelImpl =
                winrt::get_self<implementation::SplitPanel>(tab->Panel());
            if (!panelImpl) continue;
            panelImpl->Tree().ForEachPane([scheme](Pane& p) {
                if (auto* tc = Tab::PaneToTerminalControl(p)) {
                    tc->Surface().SetColorScheme(scheme);
                }
            });
        }
        // Mirror the OS preference into the WinUI shell so titlebar,
        // TabView chrome, menus, and any XamlControlsResources-derived
        // brushes swap with the terminal content. Setting RequestedTheme
        // on the root Content element propagates through the visual
        // tree; anything binding to ThemeResource values updates on the
        // next layout tick. Guarded by IsLoaded to avoid touching the
        // tree before the framework has attached (WM_SETTINGCHANGE can
        // fire immediately after HWND creation).
        try {
            if (auto root = Content().try_as<
                    winrt::Microsoft::UI::Xaml::FrameworkElement>()) {
                if (root.IsLoaded()) {
                    root.RequestedTheme(scheme == GHOSTTY_COLOR_SCHEME_LIGHT
                        ? winrt::Microsoft::UI::Xaml::ElementTheme::Light
                        : winrt::Microsoft::UI::Xaml::ElementTheme::Dark);
                }
            }
        } catch (winrt::hresult_error const&) {
            // Window torn down mid-notification — next open will pick
            // up the current scheme via the Activated one-shot path.
        }
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
        // No focus-visual pre-apply is needed — UnfocusedDim is owned
        // by Tab.SetActivePane and is independent of XAML focus, so
        // tab switches don't disturb it.
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

    bool MainWindow::OwnsSurface(ghostty_surface_t surface) const noexcept
    {
        return surface != nullptr && m_tabs.FindBySurface(surface) != nullptr;
    }

    bool MainWindow::OwnsPane(PaneId id) const noexcept
    {
        return static_cast<bool>(id) && m_tabs.FindByPaneId(id).tab != nullptr;
    }

    void MainWindow::NotifySurfaceFocused(ghostty_surface_t surface) noexcept
    {
        m_activeSurface = surface;
        // Route the focus event back into the owning Tab so it can
        // update its per-tab dim invariant. SetActivePane is idempotent
        // when `leaf` is already the tab's active leaf, so the deferred
        // Focus call we issue after tab switches (which re-focuses the
        // same TerminalControl that was already active in the
        // newly-selected tab) doesn't repaint anything. The case that
        // matters is a pointer click on a non-active pane inside a
        // split — GotFocus fires, we land here, SetActivePane swaps
        // the dim across the panes.
        if (!surface) return;
        try {
            auto lookup = m_tabs.FindPaneBySurface(surface);
            if (lookup.tab && lookup.pane && lookup.tab->ActivePane() != lookup.pane) {
                lookup.tab->SetActivePane(lookup.pane);
            }
        } catch (...) {
            // Defensive: dim update is a UX nicety, never crash the
            // focus path over it.
        }
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
        // Class invariant: m_ghosttyApp is set in the constructor and
        // App outlives every MainWindow (App's `window` member is
        // destroyed before its `m_ghostty`). No null check needed.
        m_ghosttyApp->Tick();
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

    void MainWindow::TryClose()
    {
        // Ask each Tab whether any of its panes need confirmation.
        // Tab::NeedsConfirmClose owns the pane-tree walk (Tab owns
        // the tree, so the query belongs there); the surface-level
        // ghostty_surface_needs_confirm_quit already factors in the
        // `confirm-close-surface` config, so users who set it false
        // sail through without a dialog on every close.
        bool anyNeedsConfirm = false;
        for (auto& tab : m_tabs) {
            if (tab && tab->NeedsConfirmClose()) { anyNeedsConfirm = true; break; }
        }

        if (!anyNeedsConfirm) {
            RequestClose();
            return;
        }

        // XamlRoot is required for a ContentDialog to know which
        // Window / island to overlay. Fall back to a plain close
        // if the tree isn't realised yet (shouldn't happen — a
        // close reaching TryClose means the window has been shown).
        auto content = Content();
        auto xamlRoot = content ? content.XamlRoot() : nullptr;
        if (!xamlRoot) {
            RequestClose();
            return;
        }

        muxc::ContentDialog dlg;
        dlg.XamlRoot(xamlRoot);
        dlg.Title(winrt::box_value(winrt::hstring{ L"Close window?" }));
        dlg.Content(winrt::box_value(winrt::hstring{
            L"One or more terminals in this window still have running "
            L"processes. They will be terminated." }));
        dlg.PrimaryButtonText(L"Close");
        dlg.CloseButtonText(L"Cancel");
        dlg.DefaultButton(muxc::ContentDialogButton::Close);

        auto op = dlg.ShowAsync();
        auto weak = get_weak();
        op.Completed([weak](auto&& sender, auto&& status) {
            if (status != winrt::Windows::Foundation::AsyncStatus::Completed) return;
            if (sender.GetResults() != muxc::ContentDialogResult::Primary) return;
            if (auto self = weak.get()) self->RequestClose();
        });
    }

    void MainWindow::InitGhostty()
    {
        // m_ghosttyApp + m_ghosttyDispatcher are already set in the
        // constructor — both of them only need state that's
        // available at ctor time. What's left for this method is
        // m_tabFactory, which depends on the HWND fetched from the
        // Activated handler above.
        if (m_hwnd) {
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
            ghostty::Config cfg(m_ghosttyApp->ConfigHandle());
            m_tabFactory = std::make_unique<TabFactory>(
                m_ghosttyApp->Handle(),
                cfg,
                m_hwnd,
                App::g_app->PaneIds(),
                std::move(onLeafFocused));
        }
    }

    void MainWindow::CreateTab()
    {
        if (!m_hwnd) return;

        // Redirect new-tab requests to a new window when the chrome is
        // hidden AND at least one tab already exists. The tab strip is
        // part of AppTitleBar, so with chrome collapsed there's no UI
        // to switch tabs — a second tab would be invisible and
        // unreachable. Falling back to a fresh top-level window
        // matches the upstream macOS behaviour, where
        // `window-decoration=false` disables native tabs entirely and
        // new-tab requests become new windows. The first tab is exempt
        // so the terminal can come up at all when the user launches
        // with chrome already off.
        if (!m_tabs.Empty()) {
            ghostty::Config cfg(m_ghosttyApp->ConfigHandle());
            if (!m_windowDecorations.Effective(cfg.WindowDecoratedByConfig())) {
                if (App::g_app) App::g_app->CreateNewWindow();
                return;
            }
        }

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
            // Make the active panel Visible as the canonical step,
            // not as a "safeguard against SelectionChanged not firing".
            //
            // SelectionChanged on TabView only fires once the control
            // template has been realized, which is keyed off the first
            // Measure pass — and Measure doesn't run on Collapsed
            // elements. So when the chrome is hidden (AppTitleBar
            // collapsed at startup for `window-decoration=false`), the
            // SelectedItem property updates but no event fires from
            // the unrealized template. Relying on the event to drive
            // visibility is an invariant that's only true in some
            // visual-tree states; calling Update here makes it true
            // unconditionally.
            //
            // The SelectionChanged handler still calls Update for user-
            // initiated tab clicks (a different entry into the same
            // invariant). Both call sites are idempotent — same input
            // state, same output state — so doubling up on the chrome-
            // visible activation path is harmless.
            self->UpdateActivePanelVisibility();
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
        // Values are PHYSICAL pixels (see display::MeasuredPhysical for
        // why the conversion matters). First-tab case: ActiveControl()
        // is null and AppContent has already been measured (Activated
        // fires after the first layout pass), so the AppContent
        // fallback gives a non-zero hint.
        uint32_t initialW = 0, initialH = 0;
        if (auto* prevControl = ActiveControl()) {
            auto sz = display::MeasuredPhysical(prevControl->InnerPanel());
            initialW = sz.width;
            initialH = sz.height;
        }
        if (initialW == 0 || initialH == 0) {
            auto sz = display::MeasuredPhysical(AppContent());
            if (initialW == 0) initialW = sz.width;
            if (initialH == 0) initialH = sz.height;
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
            // Once the shell has spoken, the foreground-pid poll
            // stops overwriting the header for this tab.
            t->MarkExplicitTitle();
        }
    }

    namespace {
        // Resolve a Win32 PID to its executable's basename without an
        // extension (e.g. 12345 -> "vim", "ssh"). Returns an empty
        // hstring when the process handle can't be opened (rights,
        // rapid exit) or the query fails — the caller falls back to
        // whatever the header already shows.
        winrt::hstring PidToBasename(uint32_t pid) noexcept {
            if (!pid) return {};
            // QUERY_LIMITED_INFORMATION is the minimum right that
            // still lets QueryFullProcessImageNameW read the image
            // path — QUERY_INFORMATION is stronger and can fail on
            // protected processes we don't need to see.
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, pid);
            if (!h) return {};
            wchar_t buf[MAX_PATH];
            DWORD size = static_cast<DWORD>(std::size(buf));
            BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &size);
            CloseHandle(h);
            if (!ok || size == 0) return {};
            // Trim to basename without ".exe". The upstream ghostty
            // tab title convention is `command` (no path, no
            // extension) so match it.
            std::wstring_view sv{ buf, size };
            auto slash = sv.find_last_of(L"\\/");
            if (slash != std::wstring_view::npos) sv.remove_prefix(slash + 1);
            constexpr std::wstring_view kExe{ L".exe" };
            if (sv.size() > kExe.size()) {
                auto tail = sv.substr(sv.size() - kExe.size());
                bool matches = true;
                for (size_t i = 0; i < kExe.size(); ++i) {
                    if (::towlower(tail[i]) != kExe[i]) { matches = false; break; }
                }
                if (matches) sv.remove_suffix(kExe.size());
            }
            return winrt::hstring(sv);
        }
    }

    void MainWindow::StartForegroundPidPoll()
    {
        if (m_foregroundPidTimer) return;  // already running
        auto dq = DispatcherQueue();
        if (!dq) return;
        m_foregroundPidTimer = dq.CreateTimer();
        m_foregroundPidTimer.Interval(std::chrono::seconds{1});
        // Weak `this`: the timer is a member (holds an unrelated
        // WinUI ref count), and MainWindow's destructor stops it,
        // but capturing weak plus swallowing an empty get() means
        // teardown edge cases can't reach into a half-torn window.
        auto weak = get_weak();
        m_foregroundPidTimer.Tick([weak](auto&&, auto&&) {
            if (auto self = weak.get()) self->UpdateForegroundNames();
        });
        m_foregroundPidTimer.Start();
    }

    void MainWindow::UpdateForegroundNames() noexcept
    {
        for (auto& tab : m_tabs) {
            if (!tab) continue;
            // Shell-supplied titles are sticky — leave them alone.
            if (tab->HasExplicitTitle()) continue;
            auto* tc = tab->ActiveControl();
            if (!tc) continue;
            uint32_t pid = tc->Surface().ForegroundPid();
            if (!pid) continue;
            // Same PID as last tick: keep whatever's already on the
            // header, no per-process work.
            if (tab->LastForegroundPid() == pid) continue;
            auto name = PidToBasename(pid);
            if (name.empty()) continue;
            tab->SetForegroundCache(pid, name);
            try {
                tab->Item().Header(winrt::box_value(name));
            } catch (winrt::hresult_error const&) {
                // Tab may be tearing down (drag between windows,
                // close race). Not our problem — next tick will
                // find it gone.
            }
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

    void MainWindow::ToggleWindowDecorations()
    {
        // Flip the override first, then re-apply. The tag owns the
        // 3-state override; Apply reads it back and translates the
        // effective state into XAML Visibility.
        ghostty::Config cfg(m_ghosttyApp->ConfigHandle());
        bool configDecorated = cfg.WindowDecoratedByConfig();
        m_windowDecorations.Toggle(configDecorated);
        ApplyWindowDecorationsAppearance();
    }

    void MainWindow::SetFloatOnTop(ghostty_action_float_window_e mode)
    {
        if (!m_hwnd) return;
        // ON / OFF are absolute; TOGGLE flips the current Z-order
        // state. GWL_EXSTYLE's WS_EX_TOPMOST is the source of truth —
        // reading it means we don't need a bool member that has to
        // stay in sync with the actual window state (something else
        // like an external always-on-top utility could have moved
        // the window in either direction).
        bool desired;
        switch (mode) {
            case GHOSTTY_FLOAT_WINDOW_ON:  desired = true;  break;
            case GHOSTTY_FLOAT_WINDOW_OFF: desired = false; break;
            case GHOSTTY_FLOAT_WINDOW_TOGGLE:
            default:
                desired = (GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0;
                break;
        }
        HWND after = desired ? HWND_TOPMOST : HWND_NOTOPMOST;
        SetWindowPos(m_hwnd, after, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void MainWindow::ApplyWindowDecorationsAppearance()
    {
        // Collapse AppTitleBar as a single unit — it wraps the entire
        // chrome row (tab strip + drag region + caption buttons) and
        // lives in its own outer-Grid Auto row, so hiding it shrinks
        // the row to 0 and AppContent expands to fill the window.
        // Matches the upstream macOS `window-decoration=false`
        // experience: chrome gone, terminal area only.
        //
        // ExtendsContentIntoTitleBar stays true unconditionally — the OS
        // native title bar was already removed at construction (#67),
        // so "undecorated" here means hiding our own custom chrome row.
        ghostty::Config cfg(m_ghosttyApp->ConfigHandle());
        bool configDecorated = cfg.WindowDecoratedByConfig();
        bool decorated = m_windowDecorations.Effective(configDecorated);
        AppTitleBar().Visibility(decorated
            ? winrt::Microsoft::UI::Xaml::Visibility::Visible
            : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
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
        // the wrong pane (#65). FindPaneBySurface walks the pane tree
        // and returns the owning leaf; in the single-pane case it
        // resolves to the same control ActiveControl() would.
        auto lookup = m_tabs.FindPaneBySurface(surface);
        if (!lookup.pane) return;
        if (auto* tc = Tab::PaneToTerminalControl(*lookup.pane)) {
            tc->SetCursorShape(shape);
        }
    }

    void MainWindow::ReplaceConfig(ghostty_config_t cloned)
    {
        m_ghosttyApp->ReplaceConfig(cloned);
    }

    void MainWindow::ReloadConfig(bool soft)
    {
        if (soft) {
            // Soft reload: re-apply the config we already hold.
            // Runs on whichever thread called us — the ghostty handles
            // are stable, and ghostty_app_update_config is thread-safe
            // on its own state. Capture `this` so the queued lambda
            // reads m_ghosttyApp from the same MainWindow.
            DispatcherQueue().TryEnqueue([this]() {
                auto app = m_ghosttyApp->Handle();
                auto cfg = m_ghosttyApp->ConfigHandle();
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
                    ghostty_app_update_config(
                        mwLocal->m_ghosttyApp->Handle(), newCfg);
                    mwLocal->m_ghosttyApp->ReplaceConfig(newCfg);
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
            if (auto lookup = m_tabs.FindPaneBySurface(surface);
                lookup.pane && lookup.pane->id)
            {
                auto idStr = std::to_wstring(lookup.pane->id.value);
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
                if (lookup.pane) {
                    lookup.tab->SetActivePane(lookup.pane);
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
        // Locate the Pane hosting `surface` in the given SplitPanel's
        // tree. Delegates to Tree::FindPane with the surface-ownership
        // predicate; the tree is small (a handful of panes at most).
        Pane* FindPaneForSurface(implementation::SplitPanel* panelImpl,
                                 ghostty_surface_t surface)
        {
            if (!panelImpl) return nullptr;
            return panelImpl->Tree().FindPane([surface](Pane const& p) {
                auto const* tc = Tab::PaneToTerminalControl(p);
                return tc && tc->Surface().Owns(surface);
            });
        }

        // Push every pane under `branch` into `out` in depth-first
        // order — left subtree before right. PREVIOUS / NEXT pane
        // navigation iterates this list to find neighbours of the
        // currently active pane.
        void CollectPanes(Branch& branch, std::vector<Pane*>& out) {
            branch.ForEachPane([&out](Pane& p) { out.push_back(&p); });
        }

        // The Branch wrapping `pane` in this tree, needed for
        // arrangedRect lookup (rects live on Branch, not Pane).
        Branch* BranchOfPane(implementation::SplitPanel* panelImpl,
                             Pane const* pane)
        {
            if (!panelImpl || !pane) return nullptr;
            auto* root = panelImpl->Tree().Root();
            return root ? root->FindBranchOfPane(pane) : nullptr;
        }

        // Pick the pane whose arranged rect is adjacent to `active`
        // in the requested cardinal direction. Filters to panes
        // strictly on the requested side, then scores them by primary
        // distance (along the axis) plus a perpendicular penalty so
        // an aligned neighbour beats a far-off-axis one.
        //
        // Returns nullptr if no candidate qualifies — caller's job
        // to decide whether to fall back (today: just ignore the
        // input, matching how Windows Terminal handles "no neighbour
        // in this direction").
        Pane* FindAdjacentPane(implementation::SplitPanel* panelImpl,
                               Pane* active,
                               std::vector<Pane*> const& panes,
                               ghostty_action_goto_split_e dir)
        {
            if (!active || !panelImpl) return nullptr;
            auto* activeBranch = BranchOfPane(panelImpl, active);
            if (!activeBranch) return nullptr;
            auto a = activeBranch->arrangedRect;
            float ax2 = a.X + a.Width;
            float ay2 = a.Y + a.Height;
            float aCenterX = a.X + a.Width  * 0.5f;
            float aCenterY = a.Y + a.Height * 0.5f;

            Pane* best = nullptr;
            double bestScore = std::numeric_limits<double>::max();
            for (auto* candidate : panes) {
                if (candidate == active) continue;
                auto* candBranch = BranchOfPane(panelImpl, candidate);
                if (!candBranch) continue;
                auto c = candBranch->arrangedRect;
                float cx2 = c.X + c.Width;
                float cy2 = c.Y + c.Height;
                float cCenterX = c.X + c.Width  * 0.5f;
                float cCenterY = c.Y + c.Height * 0.5f;

                double primary = 0.0, perpendicular = 0.0;
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
                    best = candidate;
                }
            }
            return best;
        }
    }

    void MainWindow::BroadcastOcclusion(bool visible)
    {
        for (auto& tab : m_tabs) {
            if (!tab) continue;
            auto* panelImpl =
                winrt::get_self<implementation::SplitPanel>(tab->Panel());
            if (!panelImpl) continue;
            panelImpl->Tree().ForEachPane([visible](Pane& p) {
                if (auto* tc = Tab::PaneToTerminalControl(p)) {
                    tc->Surface().SetOcclusion(visible);
                }
            });
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

        Pane* sourcePane = FindPaneForSurface(panelImpl, surface);
        if (!sourcePane) return;

        // The source pane's UIElement + PaneId get moved into a fresh
        // wrapper Pane inside the split subtree we build below.
        // Capturing them here means the wrapper has its own reference
        // to the underlying TerminalControl before the ReplacePane call
        // destroys the original Branch.
        auto sourceContent = sourcePane->content;
        PaneId sourcePaneId = sourcePane->id;

        // ghostty's split-direction maps to (direction, which-side-
        // does-the-new-pane-take). RIGHT/DOWN put the new pane after
        // the source on the layout axis; LEFT/UP put it before.
        Split::Direction splitDir;
        bool newFirst;
        switch (direction) {
            case GHOSTTY_SPLIT_DIRECTION_RIGHT: splitDir = Split::Direction::Horizontal; newFirst = false; break;
            case GHOSTTY_SPLIT_DIRECTION_LEFT:  splitDir = Split::Direction::Horizontal; newFirst = true;  break;
            case GHOSTTY_SPLIT_DIRECTION_DOWN:  splitDir = Split::Direction::Vertical;   newFirst = false; break;
            case GHOSTTY_SPLIT_DIRECTION_UP:    splitDir = Split::Direction::Vertical;   newFirst = true;  break;
            default: return;
        }

        // Size hint for the new ghostty surface: the source pane's
        // current SwapChainPanel size halved on the split axis,
        // expressed in PHYSICAL pixels (see display::MeasuredPhysical
        // for why the conversion matters).
        uint32_t srcW = 0, srcH = 0;
        if (auto* srcTc = Tab::PaneToTerminalControl(*sourcePane)) {
            auto sz = display::MeasuredPhysical(srcTc->InnerPanel());
            srcW = sz.width;
            srcH = sz.height;
        }
        uint32_t newW = (splitDir == Split::Direction::Horizontal) ? srcW / 2 : srcW;
        uint32_t newH = (splitDir == Split::Direction::Vertical)   ? srcH / 2 : srcH;

        // Wrap MakePane in an SEH guard for the same reason CreateTab
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
            std::unique_ptr<Branch> result;
        };
        SplitCtx ctx{ m_tabFactory.get(), newW, newH, nullptr };
        int ok = RunSEHGuarded([](void* arg) noexcept {
            auto* c = static_cast<SplitCtx*>(arg);
            c->result = c->factory->MakePane(c->initialWidth, c->initialHeight);
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
        auto newBranch = std::move(ctx.result);
        if (!newBranch) return;
        // Capture a stable pointer to the new Pane before newBranch
        // moves into the subtree; get_if on the variant is only
        // valid while the branch is still around.
        Pane* newPanePtr = newBranch->TryGet<Pane>();
        auto newControl = newBranch->TryGet<Pane>()
            ? newBranch->TryGet<Pane>()->content.try_as<winrt::GhosttyWin32::TerminalControl>()
            : nullptr;

        // Build the replacement subtree: a Split branch whose left/right
        // are (a) a wrapper around the original source content and
        // (b) the new pane branch, ordered per `newFirst`.
        auto sourceWrapper = MakePaneBranch(sourceContent, sourcePaneId);
        auto subtree = newFirst
            ? MakeSplitBranch(splitDir, 0.5, std::move(newBranch), std::move(sourceWrapper))
            : MakeSplitBranch(splitDir, 0.5, std::move(sourceWrapper), std::move(newBranch));

        if (!panelImpl->ReplacePane(sourcePane, std::move(subtree))) {
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
        sourceTab->SetActivePane(newPanePtr);
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
        if (panelImpl->Zoomed()) {
            panelImpl->SetZoomed(nullptr);
            return;
        }

        Pane* pane = FindPaneForSurface(panelImpl, surface);
        if (!pane) return;
        // Single-pane tabs skip the zoom — there's nothing to expand
        // against, and the visual state would be identical to the
        // normal layout. Detect by asking whether the root Branch is
        // itself the wrapping Branch for this pane.
        auto* root = panelImpl->Tree().Root();
        if (root && root->TryGet<Pane>() == pane) return;

        panelImpl->SetZoomed(pane);
        tab->SetActivePane(pane);
        // Re-focus so the zoomed pane keeps input even when zoom was
        // toggled from a non-active pane via a remapped binding.
        if (auto control = pane->content.try_as<winrt::GhosttyWin32::TerminalControl>()) {
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

        Pane* active = FindPaneForSurface(panelImpl, surface);
        if (!active) return;

        std::vector<Pane*> panes;
        if (auto* root = panelImpl->Tree().Root()) CollectPanes(*root, panes);
        if (panes.size() <= 1) return;  // nothing to navigate to

        Pane* target = nullptr;
        if (direction == GHOSTTY_GOTO_SPLIT_PREVIOUS
            || direction == GHOSTTY_GOTO_SPLIT_NEXT) {
            // Cycle through DFS order. wrap-around so the last pane's
            // NEXT lands on the first and vice versa.
            auto it = std::find(panes.begin(), panes.end(), active);
            if (it == panes.end()) return;
            size_t idx = static_cast<size_t>(std::distance(panes.begin(), it));
            size_t newIdx;
            if (direction == GHOSTTY_GOTO_SPLIT_NEXT) {
                newIdx = (idx + 1) % panes.size();
            } else {
                newIdx = (idx == 0) ? panes.size() - 1 : idx - 1;
            }
            target = panes[newIdx];
        } else {
            target = FindAdjacentPane(panelImpl, active, panes, direction);
        }
        if (!target || target == active) return;

        tab->SetActivePane(target);
        if (auto element = target->content) {
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

        Pane* pane = FindPaneForSurface(panelImpl, surface);
        if (!pane) return;

        // The split axis we're resizing matches the direction axis:
        // LEFT/RIGHT → Horizontal split, UP/DOWN → Vertical split.
        Split::Direction needDir =
            (resize.direction == GHOSTTY_RESIZE_SPLIT_LEFT
             || resize.direction == GHOSTTY_RESIZE_SPLIT_RIGHT)
            ? Split::Direction::Horizontal
            : Split::Direction::Vertical;

        // Walk up from the pane's wrapping Branch to the nearest
        // ancestor Split with the matching axis. Branch::parent
        // points at the enclosing Branch (which will be a Split
        // variant); we keep walking until we find a Split whose
        // direction matches, or run out of parents.
        Branch* node = BranchOfPane(panelImpl, pane);
        while (node && node->parent) {
            Branch* parent = node->parent;
            auto* parentSplit = parent ? parent->TryGet<Split>() : nullptr;
            if (parentSplit && parentSplit->direction == needDir) {
                node = parent;  // target: this Split branch
                break;
            }
            node = parent;
        }
        if (!node) return;
        auto* targetSplit = node->TryGet<Split>();
        if (!targetSplit || targetSplit->direction != needDir) return;

        auto rect = node->arrangedRect;
        float extent = (needDir == Split::Direction::Horizontal) ? rect.Width : rect.Height;
        float useable = std::max(1.0f,
            extent - static_cast<float>(implementation::SplitPanel::kSplitterThickness));
        double deltaRatio = static_cast<double>(resize.amount) / useable;

        // Arrow direction == direction the boundary moves, regardless
        // of which side of the split the active pane is on.
        //   * RIGHT / DOWN move the boundary toward +axis → ratio
        //     grows (first child gets larger).
        //   * LEFT / UP move the boundary toward -axis → ratio shrinks.
        bool increase = (resize.direction == GHOSTTY_RESIZE_SPLIT_RIGHT
                      || resize.direction == GHOSTTY_RESIZE_SPLIT_DOWN);

        targetSplit->ratio = ClampSplitRatio(
            targetSplit->ratio + (increase ? deltaRatio : -deltaRatio));
        panelImpl->InvalidateMeasure();
        panelImpl->InvalidateArrange();
    }

    TerminalControl* MainWindow::ControlByPaneId(PaneId id) noexcept
    {
        auto lookup = m_tabs.FindByPaneId(id);
        if (!lookup.pane) return nullptr;
        return Tab::PaneToTerminalControl(*lookup.pane);
    }

    std::unique_ptr<Tab> MainWindow::ReleaseTornOutTab(
        muxc::TabViewItem const& item)
    {
        auto* t = m_tabs.FindByItem(item);
        if (!t) return nullptr;

        // The focused-surface cache must not follow the tab out: the
        // surfaces stay alive, but they stop being *this* window's
        // surfaces. Walk the departing tab's leaves rather than just
        // its active control — m_activeSurface can point at any pane.
        if (m_activeSurface) {
            if (auto* panelImpl =
                    winrt::get_self<implementation::SplitPanel>(t->Panel())) {
                if (panelImpl->Tree().FindPane([this](Pane const& p) {
                        auto const* tc = Tab::PaneToTerminalControl(p);
                        return tc && tc->Surface().Owns(m_activeSurface);
                    })) {
                    m_activeSurface = nullptr;
                }
            }
        }

        // Unparent the visual pieces but detach nothing: the swap
        // chains keep presenting while the tab is in flight.
        RemoveTabPanelFromAppContent(*t);
        auto tv = TabView();
        if (tv) {
            uint32_t idx = 0;
            if (tv.TabItems().IndexOf(item, idx)) {
                tv.TabItems().RemoveAt(idx);
            }
        }
        return m_tabs.Extract(item);
    }

    void MainWindow::AdoptTornOutTab(std::unique_ptr<Tab> tab, int32_t index)
    {
        if (!tab) return;

        // A tear-out host adopts before its first Activated has run,
        // so the HWND may not be captured yet. IWindowNative works
        // from construction onward.
        if (!m_hwnd) {
            if (auto native = this->try_as<::IWindowNative>()) {
                native->get_WindowHandle(&m_hwnd);
            }
        }

        auto tv = TabView();
        if (!tv) return;
        auto items = tv.TabItems();
        uint32_t size = items.Size();
        uint32_t at = (index < 0 || static_cast<uint32_t>(index) > size)
            ? size
            : static_cast<uint32_t>(index);
        items.InsertAt(at, tab->Item());

        // Re-point every control at this window: IME coordinates and
        // clipboard ownership key off the host HWND, and the focused
        // callback must feed THIS window's active-surface cache. Raw
        // `this` capture is safe by the same argument as InitGhostty's
        // onLeafFocused — MainWindow outlives every control it hosts.
        if (auto* panelImpl =
                winrt::get_self<implementation::SplitPanel>(tab->Panel())) {
            panelImpl->Tree().ForEachPane([this](Pane& p) {
                if (auto* tc = Tab::PaneToTerminalControl(p)) {
                    tc->Rehost(m_hwnd, [this](ghostty_surface_t s) noexcept {
                        NotifySurfaceFocused(s);
                    });
                }
            });
        }

        // Same shape as CreateTab's post-Make sequence: parent the
        // panel collapsed, register ownership, then select — the
        // SelectionChanged handler reconciles panel visibility and
        // focus for us.
        tab->Panel().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        AppContent().Children().Append(tab->Panel());
        auto selected = tab->Item();
        m_tabs.Add(std::move(tab));
        tv.SelectedItem(selected);
        UpdateActivePanelVisibility();
        // SHOWNOACTIVATE, not SHOW: make the adopted tab visible
        // without deciding focus here. Whether the window should be
        // activated is the caller's call — the drop-outside handler
        // activates its freshly spawned host, while a merge adopts
        // into a window that is already visible and focused.
        if (m_hwnd) ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    }

    void MainWindow::CloseIfTornOutEmpty()
    {
        // Tearing the last tab out leaves an empty shell — close it,
        // matching browser behaviour. Deferred through the dispatcher
        // so window teardown never runs inside tear-out event
        // dispatch, and re-checked on arrival in case a merge landed
        // a tab here in the meantime.
        if (!m_tabs.Empty()) return;
        auto dq = DispatcherQueue();
        auto weak = get_weak();
        if (!dq) { RequestClose(); return; }
        dq.TryEnqueue([weak]() {
            if (auto self = weak.get()) {
                if (self->m_tabs.Empty()) self->RequestClose();
            }
        });
    }

    void MainWindow::CloseSurfaceByPaneId(PaneId id)
    {
        auto lookup = m_tabs.FindByPaneId(id);
        if (!lookup.tab || !lookup.pane) return;
        auto* tab = lookup.tab;
        auto* pane = lookup.pane;

        // Detach first so the surface / DComp handle are released
        // synchronously, before the Branch holding the TerminalControl
        // is destroyed.
        if (auto* tc = Tab::PaneToTerminalControl(*pane)) {
            // Clear m_activeSurface if it pointed at the surface we're
            // about to free — the focused-surface cache must never
            // outlive the underlying ghostty_surface_t. The next
            // TerminalControl::GotFocus on the retargeted sibling (or
            // a new tab) will refill the slot.
            if (tc->Surface().Owns(m_activeSurface)) m_activeSurface = nullptr;
            tc->Detach();
        }

        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(tab->Panel());
        if (!panelImpl) return;

        // Identify a sibling pane BEFORE the removal so we can
        // retarget the active pane into it (the pane pointer is about
        // to be invalidated). The sibling here means "the pane on the
        // other side of the immediate Split ancestor" — under the new
        // sum-type, we find it by walking to the wrapping Branch and
        // taking the opposite child, then picking its first pane.
        Pane* siblingPane = nullptr;
        bool closingActive = (tab->ActivePane() == pane);
        if (auto* wrapping = BranchOfPane(panelImpl, pane)) {
            if (auto* parent = wrapping->parent) {
                if (auto* parentSplit = parent->TryGet<Split>()) {
                    Branch* siblingBranch =
                        (parentSplit->left.get() == wrapping)
                            ? parentSplit->right.get()
                            : parentSplit->left.get();
                    if (siblingBranch) {
                        siblingPane = siblingBranch->FindPane(
                            [](Pane const&) { return true; });
                    }
                }
            }
        }
        // Clear the active-pane pointer up front: regardless of which
        // branch runs below, leaving it pointing at the doomed pane
        // would dangle until the SetActivePane calls overwrite it.
        if (closingActive) tab->SetActivePane(nullptr);

        auto result = panelImpl->RemovePane(pane);
        if (result == Tree::RemoveResult::Collapsed) {
            // Tab survives; retarget focus to the surviving subtree.
            if (closingActive && siblingPane) {
                tab->SetActivePane(siblingPane);
                auto element = siblingPane->content;
                if (auto control = element.try_as<winrt::GhosttyWin32::TerminalControl>()) {
                    control.Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
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
        // User-intent close → confirmation gate. TryClose walks the
        // window's panes, shows a WinUI ContentDialog when any
        // surface reports needs_confirm_quit, and only then calls
        // through to RequestClose. Alt+F4 / OS-issued WM_CLOSE still
        // bypasses this (would need a WndProc subclass), matching
        // typical Windows-app behaviour where only the app's own
        // close affordances confirm.
        TryClose();
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
