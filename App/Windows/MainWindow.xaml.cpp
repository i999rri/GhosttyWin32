#include "pch.h"
#include "Windows/MainWindow.xaml.h"
#include "App.xaml.h"
#include "Ghostty/CallbackDispatcher.h"
#include "Ghostty/Config.h"
#include "Windows/TearOut.h"
#include "Windows/TransparentBackdrop.h"
#include "Host/KeyModifiers.h"
#include "Interop/Encoding.h"
#include "Display/PhysicalPixels.h"
#include "Display/PhysicalSizeFactory.h"
#include "Win32/Clipboard.h"
#include "Win32/DebugTrace.h"
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
            // A drop-outside tear-out host adopts its tab (which
            // arms the cell-snap metrics) BEFORE this first
            // activation assigns the HWND — binding now installs
            // whatever rules were set meanwhile (#155).
            m_native.Bind(m_hwnd);
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
                                // A modal ContentDialog (rename
                                // prompt, close confirm) owns focus
                                // while it's up; restoring the
                                // terminal here would steal it and
                                // let keystrokes leak into the shell
                                // behind the dialog. Any open popup
                                // on this XamlRoot means skip — the
                                // dialog puts focus back on its own
                                // when it closes.
                                if (auto content = self->Content()) {
                                    if (auto root = content.XamlRoot()) {
                                        auto popups = winrt::Microsoft::UI::Xaml::Media::
                                            VisualTreeHelper::GetOpenPopupsForXamlRoot(root);
                                        if (popups && popups.Size() > 0) return;
                                    }
                                }
                                // An open search bar owns the keyboard
                                // the same way a dialog does: restore
                                // focus to its input box, not to the
                                // terminal behind it (#171 review —
                                // alt-tab away and back mid-search
                                // dropped focus on the pty).
                                if (auto* tc = self->ActiveControl()) {
                                    if (tc->FocusSearchIfOpen()) return;
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
            // The DataPackage deliberately stays empty (operation
            // only): the dragged tab travels through App's TabDrag
            // instead (see App::TabDrag() for why OLE can't carry it),
            // and an empty package gives foreign drop targets — a
            // text editor, say — nothing to accept, so they can't
            // swallow the drag away from TabDroppedOutside.
            tv.TabDragStarting([](auto const&, auto const& args) {
                App::g_app->TabDrag().Begin(App::g_app->PressedTab().Take());
                args.Data().RequestedOperation(
                    winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
            });

            tv.TabDragCompleted([](auto const&, auto const&) {
                App::g_app->TabDrag().End();
            });

            tv.TabStripDragOver([](auto const&, auto const& e) {
                if (App::g_app->TabDrag().InFlight()) {
                    e.AcceptedOperation(
                        winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
                }
            });

            tv.TabStripDrop([weakActivated](auto const& sender, auto const& e) {
                auto self = weakActivated.get();
                if (!self) return;
                // TabStripDrop is a plain DragEventHandler, so the
                // sender arrives as IInspectable, not a typed TabView.
                auto strip = sender.template try_as<muxc::TabView>();
                if (!strip) return;
                auto item = App::g_app->TabDrag().DraggedTab();
                if (!item) return;
                auto* source = App::g_app->FindWindowByTabItem(item);
                // Same-strip drops are reorders, which CanReorderTabs
                // already handled natively.
                if (!source || source == self.get()) return;
                // Insert before the first tab whose midpoint lies
                // right of the drop point. One coordinate space for
                // everything: the drop position and each tab's origin
                // are both expressed relative to the strip
                // (TransformToVisual), instead of asking GetPosition
                // for a fresh relative position per container — that
                // per-container form resolved to index 0 regardless
                // of where the drop landed, wedging the tab in first
                // place. Past every midpoint means -1: append.
                auto dropPos = e.GetPosition(strip);
                int32_t index = -1;
                auto items = strip.TabItems();
                for (uint32_t i = 0; i < items.Size(); ++i) {
                    auto container = strip.ContainerFromIndex(i)
                        .template try_as<muxc::TabViewItem>();
                    if (!container) continue;
                    auto origin = container.TransformToVisual(strip)
                        .TransformPoint({ 0, 0 });
                    float mid = origin.X
                        + static_cast<float>(container.ActualWidth()) / 2.0f;
                    if (dropPos.X < mid) {
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

            tv.TabDroppedOutside([weakActivated](auto const&, auto const&) {
                auto self = weakActivated.get();
                if (!self) return;
                // No fallback on purpose: args.Tab() would name the
                // wrong tab (see TabDrag), and tearing out the wrong
                // tab is worse than doing nothing.
                auto item = App::g_app->TabDrag().TakeLastDraggedTab();
                if (!item) return;
                POINT cursor{};
                std::optional<POINT> dropPoint;
                if (GetCursorPos(&cursor)) dropPoint = cursor;
                TearOut::ToNewWindow(*self, item, dropPoint,
                    [](WindowState::Inherited const& inherited) {
                        return App::g_app->CreateTearOutWindow(inherited);
                    });
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

            tv.TabCloseRequested([this](muxc::TabView const&, muxc::TabViewTabCloseRequestedEventArgs const& args) {
                auto item = args.Tab();
                auto* t = m_tabs.FindByItem(item);
                if (!t) return;
                auto content = Content();
                auto xamlRoot = content ? content.XamlRoot() : nullptr;
                auto weak = get_weak();
                m_closeGate.Submit(
                    WindowCloseGate::Scope::Tab,
                    std::move(xamlRoot),
                    [t]() { return t->NeedsConfirmClose(); },
                    [weak, item]() {
                        if (auto self = weak.get()) self->CloseTabByItem(item);
                    });
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
            HookCloseGate();
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
        // Disarm dispatched ghostty callbacks first: work already
        // queued on the UI thread runs after this destructor and
        // would touch the destroyed view (issue #131 — AV in a
        // dispatched OnMouseShape after a fast Alt+F4 close). The
        // destructor is the one point every close path passes
        // through, including the direct Close() calls that skip
        // RequestClose.
        if (m_ghosttyDispatcher) m_ghosttyDispatcher->DetachActions();
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
        // Parked tabs die alongside the live ones: Shutdown stops
        // the expiry timers, then each ~Tab runs its DetachAll
        // catch-all — same contract as m_tabs.Clear below.
        m_parkedTabs.Shutdown();
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
        constexpr UINT_PTR kCloseGateSubclassId   = 3;

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

        // Alt+F4 and any other OS-issued WM_CLOSE bypass WinUI's
        // event routing. Without an intercept the default handler
        // destroys the window synchronously — no confirmation, and
        // Actions callbacks queued on the dispatcher blow up on a
        // dangling m_view (see issue #126). This subclass turns
        // every WM_CLOSE into a gate submission; the gate's
        // approval path sets m_bypassCloseGate so the follow-up
        // Close() gets through this proc unimpeded.
        LRESULT CALLBACK CloseGateSubclassProc(
            HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
            UINT_PTR /*id*/, DWORD_PTR ref) noexcept
        {
            if (msg == WM_CLOSE) {
                auto* self = reinterpret_cast<MainWindow*>(ref);
                if (self && !self->IsCloseGateBypassed()) {
                    try { self->TryClose(); } catch (winrt::hresult_error const&) {}
                    return 0;
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

    void MainWindow::HookCloseGate() noexcept
    {
        if (!m_hwnd) return;
        SetWindowSubclass(m_hwnd, &CloseGateSubclassProc,
                          kCloseGateSubclassId,
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
                if (p.view) p.view->Surface().SetColorScheme(scheme);
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

    bool MainWindow::IsActiveSurface(ghostty_surface_t surface) noexcept
    {
        if (!surface) return false;
        try {
            auto* tc = ActiveControl();
            return tc && tc->Surface().Owns(surface);
        } catch (winrt::hresult_error const&) {
            // TabView() throws RO_E_CLOSED on a disposed window.
            return false;
        }
    }

    void MainWindow::NotifySurfaceFocused(ghostty_surface_t surface) noexcept
    {
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
        // Every RequestClose is a committed close — the confirmation
        // (if any) already happened upstream. Setting the bypass here
        // means the CloseGate subclass won't re-prompt on whatever
        // WM_CLOSE the framework emits during Close(), and covers
        // paths that call RequestClose without going through TryClose
        // (last-tab-close, tear-out empty shell, driver-error exit).
        m_bypassCloseGate = true;
        // WinUI's Window::Close throws when the window has already
        // begun tearing down; swallow so callers can fire and
        // forget. The hresult_error variant is the only one that
        // surfaces in practice (RPC_E_DISCONNECTED via the dispose
        // path).
        try { Close(); } catch (winrt::hresult_error const&) {}
    }

    void MainWindow::TryClose()
    {
        auto content = Content();
        auto xamlRoot = content ? content.XamlRoot() : nullptr;
        auto weak = get_weak();
        m_closeGate.Submit(
            WindowCloseGate::Scope::Window,
            std::move(xamlRoot),
            // Any pane in any tab reporting needs_confirm_quit means
            // we should prompt. Tab owns its own tree walk so the
            // window-level query is a linear scan over tabs.
            [this]() {
                for (auto& tab : m_tabs) {
                    if (tab && tab->NeedsConfirmClose()) return true;
                }
                return false;
            },
            // Weak ref because the dialog can outlive the C++ object
            // if teardown starts while it's still up. RequestClose
            // itself flips the bypass, so any WM_CLOSE the framework
            // re-emits during Close() sails past the subclass.
            [weak]() {
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
            // Window-state initialization for every new leaf: the
            // background-opacity mode is window-scoped (#69), and a
            // pane born after a toggle must match its window.
            auto onLeafCreated = [this](implementation::TerminalControl& tc) {
                tc.SetOpaqueBackground(
                    BackgroundOpacityAppearance().paneUnderlay, m_bgColor);
            };
            // Hand the factory the App wrapper, not a Config snapshot:
            // the config handle is freed and swapped on every config
            // change (reload, theme follow), so the factory must
            // re-resolve it per Make/MakePane call.
            m_tabFactory = std::make_unique<TabFactory>(
                *m_ghosttyApp,
                m_hwnd,
                App::g_app->PaneIds(),
                std::move(onLeafFocused),
                std::move(onLeafCreated));
            // Seed the tracked background colour from config so the
            // opaque underlay has a real colour before the first
            // COLOR_CHANGE arrives, then apply the opacity mode once
            // so a translucent config gets its backdrop/root state
            // from the first frame instead of waiting for the first
            // COLOR_CHANGE.
            m_bgColor = ghostty::Config(m_ghosttyApp->ConfigHandle()).Background();
            ApplyBackgroundOpacityAppearance();
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
        // with chrome already off. The window stands in for a tab of
        // this one, so it starts from this window's inherited state
        // (chrome override, background-opacity mode) rather than the
        // defaults.
        if (!m_tabs.Empty()) {
            ghostty::Config cfg(m_ghosttyApp->ConfigHandle());
            if (!m_state.inherited.windowDecorations.Effective(cfg.WindowDecoratedByConfig())) {
                if (App::g_app) App::g_app->CreateNewWindow(m_state.inherited);
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
        // Record header presses so TabDrag::Begin knows which tab a
        // drag started on (TabView's own args can't say — see
        // TabDrag). handledEventsToo: the inner ListViewItem marks
        // the press handled before it would reach a plain subscriber.
        // The handler travels with the item across tear-out windows.
        item.AddHandler(
            UIElement::PointerPressedEvent(),
            box_value(Input::PointerEventHandler(
                [](winrt::Windows::Foundation::IInspectable const& s,
                   Input::PointerRoutedEventArgs const&) {
                    if (auto tab = s.try_as<muxc::TabViewItem>()) {
                        App::g_app->PressedTab().Record(tab);
                    }
                })),
            /*handledEventsToo*/ true);
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
        // First-tab case: ActiveControl() is null and AppContent has
        // already been measured (Activated fires after the first
        // layout pass), so the content-only overload gives a non-zero
        // hint.
        auto* prevControl = ActiveControl();
        const auto initial = prevControl
            ? display::PhysicalSizeFactory::ForNewTab(prevControl->InnerPanel(), AppContent())
            : display::PhysicalSizeFactory::ForNewTab(AppContent());

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
        CreateCtx ctx{ &item, m_tabFactory.get(), std::move(onActivated), initial.width, initial.height, nullptr };
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
        auto* t = m_tabs.FindBySurface(surface);
        if (!t) return;
        auto item = t->Item();
        auto content = Content();
        auto xamlRoot = content ? content.XamlRoot() : nullptr;
        auto weak = get_weak();
        m_closeGate.Submit(
            WindowCloseGate::Scope::Tab,
            std::move(xamlRoot),
            [t]() { return t->NeedsConfirmClose(); },
            [weak, item]() {
                if (auto self = weak.get()) self->CloseTabByItem(item);
            });
    }

    void MainWindow::CloseTabByItem(muxc::TabViewItem const& item)
    {
        auto* t = m_tabs.FindByItem(item);
        if (!t) return;
        if (TryParkTab(*t)) return;
        TearDownTab(*t);
    }

    bool MainWindow::TryParkTab(Tab& tab)
    {
        // Undo support (#151): instead of tearing the tab down, park
        // it alive for undo-timeout. Only when another tab remains —
        // closing the last tab closes the window, and window-close
        // undo is out of scope for stage 1. undo-timeout = 0 opts
        // out entirely.
        if (!m_ghosttyApp || TabView().TabItems().Size() <= 1) return false;
        uint64_t timeoutMs =
            ghostty::Config(m_ghosttyApp->ConfigHandle()).UndoTimeoutMs();
        if (timeoutMs == 0) return false;
        ParkTab(tab.Item(), timeoutMs, /*fromRedo=*/false);
        return true;
    }

    void MainWindow::TearDownTab(Tab& tab)
    {
        auto item = tab.Item();
        // A stale press record must not keep the closed item alive.
        App::g_app->PressedTab().Forget(item);
        // Detach every pane before RemoveAt: SetSwapChainHandle(nullptr)
        // AVs at +0x1F8 inside microsoft.ui.xaml.dll if the panel has
        // already been unparented. Multi-pane tabs have multiple swap
        // chains and each one needs clearing before the panel comes
        // out of the live visual tree. Idempotent against panes a
        // caller already detached.
        tab.DetachAll();
        auto tv = TabView();
        uint32_t idx = 0;
        if (tv.TabItems().IndexOf(item, idx)) {
            tv.TabItems().RemoveAt(idx);
        }
        DwmFlush();
        if (tv.TabItems().Size() == 0) {
            // Defer Tab destruction to ~MainWindow's m_tabs.Clear:
            // tearing down the focused control synchronously here
            // leaves XAML's focus subsystem holding a stale pointer
            // that AVs at +0x1F8 once the window teardown starts.
            // RequestClose (not Close) so the close gate's bypass is
            // set and a WM_CLOSE re-emitted by the framework does not
            // re-prompt.
            RequestClose();
        } else {
            // ~Tab doesn't know about AppContent — unparent here or
            // the panel leaks as an orphan child.
            RemoveTabPanelFromAppContent(tab);
            m_tabs.Remove(item);
            DebugAssertPanelInvariant();
        }
    }

    void MainWindow::DebugAssertPanelInvariant() noexcept
    {
#if defined(_DEBUG)
        unsigned panels = 0;
        try {
            for (auto const& child : AppContent().Children()) {
                if (child.try_as<winrt::GhosttyWin32::SplitPanel>()) ++panels;
            }
        } catch (winrt::hresult_error const&) {
            return;  // window already tearing down; nothing to check
        }
        const auto tabs = static_cast<unsigned>(m_tabs.Size());
        const auto parked = static_cast<unsigned>(m_parkedTabs.Size());
        DEBUG_TRACE(L"PanelInvariant: tabs=%u parked=%u panels=%u\n",
                    tabs, parked, panels);
        if (panels != tabs + parked && IsDebuggerPresent()) __debugbreak();
#endif
    }

    void MainWindow::ParkTab(muxc::TabViewItem const& item,
                             uint64_t timeoutMs, bool fromRedo)
    {
        auto* t = m_tabs.FindByItem(item);
        if (!t) return;
        // Same bookkeeping as the immediate-teardown path: a stale
        // press record must not keep the item alive.
        App::g_app->PressedTab().Forget(item);
        auto tv = TabView();
        uint32_t idx = 0;
        if (tv.TabItems().IndexOf(item, idx)) {
            tv.TabItems().RemoveAt(idx);
        }
        // The panel stays parented under AppContent — no Detach, no
        // unparent, so the swap chains and surfaces stay live and
        // the +0x1F8 ordering contract never comes into play. It
        // just goes Collapsed, exactly like an unselected tab; with
        // its item gone from the strip, UpdateActivePanelVisibility
        // will never pick it again until Undo re-lists it.
        t->Panel().Visibility(Visibility::Collapsed);
        auto tab = m_tabs.Extract(item);
        if (!tab) return;
        // A fresh user-initiated close invalidates redo history
        // (standard undo semantics); a redo-initiated park is the
        // redo history being consumed, not new history.
        if (!fromRedo) m_parkedTabs.ClearRedoCandidates();
        // On expiry: the real teardown, with the same ordering
        // contract as the immediate close path — DetachAll while
        // the (collapsed) panel is still in the live visual tree,
        // then unparent, then destroy. If the window died first,
        // ~Tab's DetachAll catch-all still runs.
        auto weak = get_weak();
        m_parkedTabs.Park(
            std::move(tab), idx, timeoutMs, DispatcherQueue(),
            [weak](std::unique_ptr<Tab> expired) {
                if (auto self = weak.get()) {
                    expired->DetachAll();
                    self->RemoveTabPanelFromAppContent(*expired);
                    self->DebugAssertPanelInvariant();
                }
            });
        DebugAssertPanelInvariant();
    }

    void MainWindow::Undo()
    {
        auto restored = m_parkedTabs.PopNewest();
        if (!restored) return;
        auto tv = TabView();
        auto item = restored->tab->Item();
        // Tab-strip position at close time, clamped in case other
        // tabs closed meanwhile.
        uint32_t idx = std::min(restored->index, tv.TabItems().Size());
        m_tabs.Add(std::move(restored->tab));
        tv.TabItems().InsertAt(idx, item);
        m_parkedTabs.RememberRedoCandidate(item);
        // Selecting the restored tab drives the rest through the
        // canonical paths: SelectionChanged flips its panel back to
        // Visible (UpdateActivePanelVisibility) and refocuses.
        tv.SelectedIndex(static_cast<int32_t>(idx));
        // Parked panes sat out any appearance changes (background-
        // opacity toggle, recolour) — restate the window state over
        // the whole tab set, same as AdoptTornOutTab does.
        ApplyBackgroundOpacityAppearance();
        DebugAssertPanelInvariant();
    }

    void MainWindow::Redo()
    {
        uint64_t timeoutMs = 0;
        if (m_ghosttyApp) {
            timeoutMs =
                ghostty::Config(m_ghosttyApp->ConfigHandle()).UndoTimeoutMs();
        }
        if (timeoutMs == 0) return;
        while (auto item = m_parkedTabs.PopRedoCandidate()) {
            // The tab may have been closed for real (or its window
            // died) since the undo — skip to the next candidate.
            if (!m_tabs.FindByItem(*item)) continue;
            // Last-tab guard, same as CloseTabByItem's park branch.
            if (TabView().TabItems().Size() <= 1) return;
            // No close gate here: redo re-applies a close that
            // already passed the gate once, and parking doesn't
            // kill anything the gate protects.
            ParkTab(*item, timeoutMs, /*fromRedo=*/true);
            return;
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
            // A user-chosen name (rename prompt) outranks the shell:
            // upstream documents the prompt title as overriding any
            // terminal-set title, and shells re-assert their OSC
            // title on every prompt — honouring it here would undo
            // the rename within seconds.
            if (t->HasUserTitle()) return;
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
        m_state.sizeLimit.Apply(limit);
        m_native.SetSizeLimit(m_state.sizeLimit);
    }

    void MainWindow::ApplyCellSizeForSurface(ghostty_surface_t surface,
                                             ghostty_action_cell_size_s cell)
    {
        // MainWindowRuntime routes surface-targeted actions to the
        // owning window, so this call already lands on the right
        // MainWindow; the FindBySurface guard only screens the brief
        // creation window before the tab is registered. Panes within
        // one window share the font config, so whichever pane
        // reported last is the right step anyway.
        if (!m_tabs.FindBySurface(surface)) {
            DEBUG_TRACE(L"CellSnap[%llu]: CELL_SIZE %ux%u for surface=%p "
                            L"not in this window, skipped\n",
                            GetTickCount64() % 100'000, cell.width,
                            cell.height, static_cast<void*>(surface));
            return;
        }
        ArmCellSnap(cell);
    }

    void MainWindow::ArmCellSnap(ghostty_action_cell_size_s cell)
    {
        m_state.cellSize.Apply(cell, WindowStepResizeByConfig());
        m_native.SetCellSnap(m_state.cellSize);
        DEBUG_TRACE(L"CellSnap[%llu]: applied %ux%u enabled=%d\n",
                        GetTickCount64() % 100'000, cell.width,
                        cell.height, m_state.cellSize.Enabled() ? 1 : 0);
    }

    bool MainWindow::WindowStepResizeByConfig() const
    {
        if (!m_ghosttyApp) return false;
        return ghostty::Config(m_ghosttyApp->ConfigHandle()).WindowStepResize();
    }

    void MainWindow::ToggleFullscreen()
    {
        using Transition = ghostty::actions::tags::Fullscreen::Transition;
        switch (m_state.fullscreen.Toggle()) {
            case Transition::Enter: m_native.EnterFullscreen(); break;
            case Transition::Leave: m_native.LeaveFullscreen(); break;
        }
    }

    void MainWindow::ToggleWindowDecorations()
    {
        // Flip the override first, then re-apply. The tag owns the
        // 3-state override; Apply reads it back and translates the
        // effective state into XAML Visibility.
        ghostty::Config cfg(m_ghosttyApp->ConfigHandle());
        bool configDecorated = cfg.WindowDecoratedByConfig();
        m_state.inherited.windowDecorations.Toggle(configDecorated);
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
        bool decorated = m_state.inherited.windowDecorations.Effective(configDecorated);
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

    // ----- IWindow: surface directory + terminal-driven appearance -----

    host::ISurfaceView* MainWindow::FindSurfaceView(ghostty_surface_t surface)
    {
        // Owning-leaf resolution for every surface-targeted action.
        // Null when the surface was closed or torn out to another
        // window before the dispatched call landed — the caller
        // drops the action, as the old per-action relays did.
        auto lookup = m_tabs.FindPaneBySurface(surface);
        if (!lookup.pane) return nullptr;
        return ControlOf(*lookup.pane);
    }

    void MainWindow::ApplyBackgroundColor(uint8_t r, uint8_t g, uint8_t b)
    {
        if (m_hwnd) {
            COLORREF color = RGB(r, g, b);
            DwmSetWindowAttribute(m_hwnd, DWMWA_CAPTION_COLOR, &color, sizeof(color));
            float luminance = 0.299f * r + 0.587f * g + 0.114f * b;
            COLORREF textColor = (luminance < 128) ? RGB(255, 255, 255) : RGB(0, 0, 0);
            DwmSetWindowAttribute(m_hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));
        }
        m_bgColor = winrt::Windows::UI::Color{ 255, r, g, b };
        // Root painting + per-pane underlays both depend on the
        // background-opacity mode, so a recolour funnels through the
        // same apply as the toggle (#69).
        ApplyBackgroundOpacityAppearance();
    }

    void MainWindow::ToggleBackgroundOpacity()
    {
        if (!m_ghosttyApp) return;
        ghostty::Config cfg(m_ghosttyApp->ConfigHandle());
        // The tag holds the guards (config already opaque, or
        // fullscreen); a guarded toggle changes nothing to re-apply.
        if (!m_state.inherited.backgroundOpacity.Toggle(cfg.BackgroundOpacity(),
                                              m_state.fullscreen.Active())) {
            return;
        }
        ApplyBackgroundOpacityAppearance();
    }

    ghostty::actions::tags::BackgroundOpacity::Appearance
    MainWindow::BackgroundOpacityAppearance() const
    {
        // No ghostty yet = nothing configured = opaque, same as a
        // config with background-opacity 1.0.
        double opacity = 1.0;
        bool blur = false;
        if (m_ghosttyApp) {
            ghostty::Config cfg(m_ghosttyApp->ConfigHandle());
            opacity = cfg.BackgroundOpacity();
            blur = cfg.BackgroundBlurEnabled();
        }
        return m_state.inherited.backgroundOpacity.Effective(opacity, blur);
    }

    void MainWindow::ApplyBackgroundOpacityAppearance()
    {
        // Every branch below is a decision the tag already made
        // (see BackgroundOpacity.h for why each backdrop); this
        // method only turns it into XAML / DWM calls.
        using Backdrop = ghostty::actions::tags::BackgroundOpacity::Backdrop;
        const auto look = BackgroundOpacityAppearance();
        try {
            switch (look.backdrop) {
                case Backdrop::ClearAcrylic:
                    SystemBackdrop(winrt::GhosttyWin32::ClearAcrylicBackdrop());
                    break;
                case Backdrop::Transparent:
                    SystemBackdrop(winrt::GhosttyWin32::TransparentBackdrop());
                    break;
                case Backdrop::Mica:
                    SystemBackdrop(winrt::Microsoft::UI::Xaml::Media::MicaBackdrop());
                    break;
            }
        } catch (winrt::hresult_error const&) {
            // Backdrop swap can fail during teardown; visuals only.
        }
        // DWM composites a window's surface as opaque by default, so
        // the transparent backdrop alone renders BLACK, not
        // see-through. Enabling "blur behind" with an empty region is
        // the long-standing switch that makes DWM honour the window's
        // per-pixel alpha (the blur itself has been a no-op since
        // Win8; only the alpha semantics remain).
        if (m_hwnd) {
            DWM_BLURBEHIND bb{};
            bb.dwFlags = DWM_BB_ENABLE;
            bb.fEnable = look.dwmPerPixelAlpha ? TRUE : FALSE;
            HRGN rgn = nullptr;
            if (look.dwmPerPixelAlpha) {
                rgn = CreateRectRgn(-1, -1, 0, 0);
                bb.dwFlags |= DWM_BB_BLURREGION;
                bb.hRgnBlur = rgn;
            }
            DwmEnableBlurBehindWindow(m_hwnd, &bb);
            if (rgn) DeleteObject(rgn);
        }
        if (auto content = Content()) {
            auto panel = content.as<winrt::Microsoft::UI::Xaml::Controls::Panel>();
            if (look.paintRoot) {
                panel.Background(
                    winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(m_bgColor));
            } else {
                panel.Background(nullptr);
            }
        }
        for (auto& tab : m_tabs) {
            if (tab) tab->ApplyBackgroundOpacity(look.paneUnderlay, m_bgColor);
        }
    }

    namespace {
        // "2m 5s" / "12s" style, for the command-finished toast body.
        // Sub-second commands rarely qualify (the default threshold
        // is 5s), so second granularity is enough.
        winrt::hstring FormatDurationNs(uint64_t ns) {
            const uint64_t totalSec = ns / 1'000'000'000ull;
            wchar_t buf[64];
            if (totalSec >= 3600) {
                swprintf_s(buf, L"%lluh %llum",
                           totalSec / 3600, (totalSec % 3600) / 60);
            } else if (totalSec >= 60) {
                swprintf_s(buf, L"%llum %llus",
                           totalSec / 60, totalSec % 60);
            } else {
                swprintf_s(buf, L"%llus", totalSec);
            }
            return winrt::hstring{ buf };
        }
    }

    void MainWindow::NotifyCommandFinishedForSurface(ghostty_surface_t surface,
                                                     int exitCode,
                                                     uint64_t durationNs)
    {
        // Policy mirrors macOS commandFinished: config gate first
        // (mode, then duration threshold), then the configured
        // actions. Config is read fresh per event so a reload takes
        // effect without replumbing.
        core::ghostty::Config cfg{ m_ghosttyApp->ConfigHandle() };
        using Notify = core::ghostty::Config::NotifyOnCommandFinish;
        const auto mode = cfg.CommandFinishNotify();
        if (mode == Notify::Never) return;
        if (mode == Notify::Unfocused) {
            // "Focused" means the pane is this window's active
            // surface AND the window is in the foreground — a
            // command finishing in a visible, focused pane needs no
            // announcement.
            const bool focused = IsActiveSurface(surface) &&
                                 GetForegroundWindow() == m_hwnd;
            if (focused) return;
        }
        if (durationNs < cfg.CommandFinishNotifyAfterNs()) return;

        const auto actions = cfg.CommandFinishNotifyActions();
        if (actions.bell) MessageBeep(MB_OK);
        if (actions.notify) {
            // Title wording matches upstream macOS; exitCode < 0
            // means the shell didn't report one.
            std::wstring title = exitCode < 0 ? L"Command Finished"
                                : exitCode == 0 ? L"Command Succeeded"
                                                : L"Command Failed";
            std::wstring body = L"Finished in ";
            body += FormatDurationNs(durationNs);
            if (exitCode >= 0) {
                body += L" (exit ";
                body += std::to_wstring(exitCode);
                body += L")";
            }
            ShowDesktopNotification(surface, std::move(title), std::move(body));
        }
    }

    void MainWindow::PromptTitleForSurface(ghostty_surface_t surface)
    {
        // One ContentDialog per XamlRoot is a WinUI rule; a second
        // ShowAsync throws. If a rename prompt (or the close-confirm
        // dialog) is already up, dropping the request matches how
        // repeated keypresses should feel anyway.
        if (m_renamePromptOpen) return;
        auto* t = m_tabs.FindBySurface(surface);
        if (!t) return;
        auto content = Content();
        auto xamlRoot = content ? content.XamlRoot() : nullptr;
        if (!xamlRoot) return;

        muxc::TextBox input;
        input.Text(unbox_value_or<winrt::hstring>(t->Item().Header(), L""));
        // Pre-select so typing replaces the old title outright —
        // the common case is a full rename, not an edit.
        input.SelectAll();

        muxc::ContentDialog dlg;
        dlg.XamlRoot(xamlRoot);
        dlg.Title(box_value(L"Rename tab"));
        dlg.Content(input);
        dlg.PrimaryButtonText(L"Rename");
        dlg.CloseButtonText(L"Cancel");
        dlg.DefaultButton(muxc::ContentDialogButton::Primary);

        m_renamePromptOpen = true;
        auto op = dlg.ShowAsync();
        // Weak ref: the dialog can outlive this window if teardown
        // starts while it's up (same rationale as WindowCloseGate).
        // The TabViewItem is captured by value — WinRT projections
        // are refcounted handles, so applying the rename to a tab
        // that got torn out or closed mid-prompt is a safe no-op on
        // a still-alive object rather than a dangling pointer.
        auto weak = get_weak();
        auto item = t->Item();
        op.Completed([weak, item, input](auto const& sender, auto const&) {
            auto self = weak.get();
            if (self) self->m_renamePromptOpen = false;
            if (sender.GetResults() != muxc::ContentDialogResult::Primary) return;
            auto text = input.Text();
            // Empty input is treated as cancel: this port has no
            // inverse of MarkExplicitTitle yet, so "reset to the
            // automatic title" can't be honoured truthfully.
            if (text.empty()) return;
            item.Header(box_value(text));
            if (self) {
                if (auto* tab = self->m_tabs.FindByItem(item)) {
                    // User latch: outranks both the foreground-pid
                    // poll AND shell SET_TITLE (see HasUserTitle) —
                    // shells re-assert their OSC title constantly,
                    // so anything weaker gets undone in seconds.
                    tab->MarkUserTitle();
                }
            }
        });
    }

    // ----- search bar: owning-leaf routing for all four actions -----

    void MainWindow::SetPwdForSurface(ghostty_surface_t surface, std::wstring pwd)
    {
        // Tab-level, not pane-level: the tooltip hangs off the
        // TabViewItem. With splits the last pane to report wins —
        // "the directory the user last worked in".
        auto* t = m_tabs.FindBySurface(surface);
        if (!t) return;
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
            t->Item(),
            pwd.empty() ? nullptr : box_value(winrt::hstring{ pwd }));
    }

    void MainWindow::ReplaceConfig(ghostty_config_t cloned)
    {
        m_ghosttyApp->ReplaceConfig(cloned);
        // window-step-resize can be toggled by itself; CELL_SIZE
        // only re-fires on metric changes, so re-read the gate here
        // so a reload flips snapping immediately (#155).
        m_state.cellSize.SetEnabled(WindowStepResizeByConfig());
        m_native.SetCellSnap(m_state.cellSize);
        DEBUG_TRACE(L"CellSnap[%llu]: config replaced, enabled=%d\n",
                        GetTickCount64() % 100'000,
                        m_state.cellSize.Enabled() ? 1 : 0);
        // background-opacity / background-blur likewise: a reload
        // only brings CONFIG_CHANGE (COLOR_CHANGE is the OSC path),
        // so nothing else re-reads them until the next toggle.
        ApplyBackgroundOpacityAppearance();
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

    void MainWindow::BroadcastOcclusion(bool visible)
    {
        for (auto& tab : m_tabs) {
            if (!tab) continue;
            auto* panelImpl =
                winrt::get_self<implementation::SplitPanel>(tab->Panel());
            if (!panelImpl) continue;
            panelImpl->Tree().ForEachPane([visible](Pane& p) {
                if (p.view) p.view->Surface().SetOcclusion(visible);
            });
        }
    }

    void MainWindow::SplitActivePane(ghostty_surface_t surface,
                                     ghostty_action_split_direction_e direction)
    {
        if (!m_tabFactory || !surface) return;
        auto lookup = m_tabs.FindPaneBySurface(surface);
        if (!lookup.pane) return;
        auto* sourceTab = lookup.tab;
        Pane* sourcePane = lookup.pane;
        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(sourceTab->Panel());
        if (!panelImpl) return;
        auto splitDirection = Split::Direction::From(direction);
        if (!splitDirection) return;

        display::PhysicalSize hint{};
        if (auto* srcTc = ControlOf(*sourcePane)) {
            hint = display::PhysicalSizeFactory::ForSplit(srcTc->InnerPanel(), *splitDirection);
        }

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
        SplitCtx ctx{ m_tabFactory.get(), hint.width, hint.height, nullptr };
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
        auto fresh = std::move(ctx.result);
        if (!fresh) return;
        // Keep the new pane's handle and view: if the tree mutation
        // fails the branch is destroyed and the attached surface must
        // be detached rather than leaked (the handle keeps the
        // control alive for that call).
        winrt::Windows::Foundation::IInspectable newHandle{ nullptr };
        host::IPaneView* newView = nullptr;
        if (auto* p = fresh->TryGet<Pane>()) { newHandle = p->handle; newView = p->view; }

        Pane* created = panelImpl->SplitPane(*sourcePane, *splitDirection, std::move(fresh));
        if (!created) {
            if (newView) newView->Detach();
            return;
        }

        // Focus shifts to the freshly-created pane: matches the
        // expectation set by every other terminal (a `:vsplit` lands
        // the cursor in the new pane).
        sourceTab->SetActivePane(created);
        if (newView) newView->TakeFocus();
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
        auto lookup = m_tabs.FindPaneBySurface(surface);
        if (!lookup.pane) return;
        auto* tab = lookup.tab;
        Pane* pane = lookup.pane;
        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(tab->Panel());
        if (!panelImpl) return;

        if (!panelImpl->ToggleZoom(*pane)) return;
        tab->SetActivePane(pane);
        // Re-focus so the zoomed pane keeps input even when zoom was
        // toggled from a non-active pane via a remapped binding.
        if (pane->view) pane->view->TakeFocus();
    }

    void MainWindow::GotoSplitFromAction(ghostty_surface_t surface,
                                         ghostty_action_goto_split_e direction)
    {
        if (!surface) return;
        auto lookup = m_tabs.FindPaneBySurface(surface);
        if (!lookup.pane) return;
        auto* tab = lookup.tab;
        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(tab->Panel());
        if (!panelImpl) return;

        Pane* target = panelImpl->PaneToward(*lookup.pane, direction);
        if (!target || target == lookup.pane) return;

        tab->SetActivePane(target);
        if (target->view) target->view->TakeFocus();
    }

    void MainWindow::ResizeSplitFromAction(ghostty_surface_t surface,
                                           ghostty_action_resize_split_s resize)
    {
        if (!surface) return;
        auto lookup = m_tabs.FindPaneBySurface(surface);
        if (!lookup.pane) return;
        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(lookup.tab->Panel());
        if (!panelImpl) return;
        panelImpl->ResizeSplit(*lookup.pane, resize);
    }

    TerminalControl* MainWindow::ControlByPaneId(PaneId id) noexcept
    {
        auto lookup = m_tabs.FindByPaneId(id);
        if (!lookup.pane) return nullptr;
        return ControlOf(*lookup.pane);
    }

    std::unique_ptr<Tab> MainWindow::ReleaseTornOutTab(
        muxc::TabViewItem const& item)
    {
        auto* t = m_tabs.FindByItem(item);
        if (!t) return nullptr;

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
                if (auto* tc = p.view) {
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
        // Query before the move below: the adopted surfaces are the
        // source of truth for their own cell metrics
        // (ghostty_surface_size), no carried state needed.
        ghostty_action_cell_size_s adoptedCell{};
        if (auto* tc = tab->ActiveControl()) {
            auto size = tc->Surface().Size();
            adoptedCell = { size.cell_width_px, size.cell_height_px };
        }
        m_tabs.Add(std::move(tab));
        tv.SelectedItem(selected);
        UpdateActivePanelVisibility();
        // SHOWNOACTIVATE, not SHOW: make the adopted tab visible
        // without deciding focus here. Whether the window should be
        // activated is the caller's call — the drop-outside handler
        // activates its freshly spawned host, while a merge adopts
        // into a window that is already visible and focused.
        if (m_hwnd) ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        // The adopted panes carry the previous window's background-
        // opacity mode (#69 — window-scoped state); restate this
        // window's mode over the whole tab set.
        ApplyBackgroundOpacityAppearance();
        // Arm resize snapping from the queried metrics: the adopted
        // surfaces already reported CELL_SIZE in their previous
        // window and ghostty won't re-report on a move, so a fresh
        // tear-out window would otherwise never snap (#155).
        ArmCellSnap(adoptedCell);
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
        auto* tc = ControlOf(*lookup.pane);
        auto content = Content();
        auto xamlRoot = content ? content.XamlRoot() : nullptr;
        auto weak = get_weak();
        m_closeGate.Submit(
            WindowCloseGate::Scope::Surface,
            std::move(xamlRoot),
            // Surface-level predicate: ask this pane alone. When the
            // shell exits naturally ghostty returns false, so
            // spontaneous close_surface_cb fires (child exit) sail
            // through without a dialog.
            [tc]() { return tc && tc->Surface().NeedsConfirmQuit(); },
            [weak, id]() {
                if (auto self = weak.get()) self->RemovePaneByIdApproved(id);
            });
    }

    void MainWindow::RemovePaneByIdApproved(PaneId id)
    {
        auto lookup = m_tabs.FindByPaneId(id);
        if (!lookup.tab || !lookup.pane) return;
        auto* tab = lookup.tab;
        auto* pane = lookup.pane;

        // Undo support (#151): when this pane is the tab's only one,
        // the close is a whole-tab close (close_surface via
        // Ctrl+Shift+W on an unsplit tab — the most common
        // accidental close) and can be parked exactly like
        // CloseTabByItem's park branch. Decided BEFORE the Detach
        // below, which frees the surface and would leave nothing to
        // restore. Two exclusions: the shell exiting on its own also
        // lands here (close_surface_cb after child exit) and a dead
        // process can't be brought back, and pane closes inside a
        // split stay immediate for stage 1 (subtree extraction is
        // its own project).
        {
            auto* panelForPark =
                winrt::get_self<implementation::SplitPanel>(tab->Panel());
            Branch* wrappingForPark =
                panelForPark ? panelForPark->Tree().TryFindBranch(*pane) : nullptr;
            bool onlyPane = wrappingForPark && !wrappingForPark->parent;
            bool processAlive =
                pane->view && !pane->view->Surface().ProcessExited();
            DEBUG_TRACE(L"UndoPark[%llu]: close-eval pane=%p wrapping=%p "
                            L"parent=%p onlyPane=%d alive=%d tabs=%u\n",
                            GetTickCount64() % 100'000,
                            static_cast<void*>(pane),
                            static_cast<void*>(wrappingForPark),
                            static_cast<void*>(
                                wrappingForPark ? wrappingForPark->parent
                                                : nullptr),
                            onlyPane ? 1 : 0, processAlive ? 1 : 0,
                            TabView().TabItems().Size());
            if (onlyPane && processAlive && TryParkTab(*tab)) return;
        }

        // Detach first so the surface / DComp handle are released
        // synchronously, before the Branch holding the TerminalControl
        // is destroyed.
        if (auto* tc = pane->view) {
            tc->Detach();
        }

        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(tab->Panel());
        if (!panelImpl) return;

        // Pick a sibling pane before removal invalidates the pointer.
        // "Sibling" == the first pane on the other child of the
        // immediate Split ancestor.
        Pane* siblingPane = nullptr;
        bool closingActive = (tab->ActivePane() == pane);
        if (auto* wrapping = panelImpl ? panelImpl->Tree().TryFindBranch(*pane) : nullptr) {
            if (auto* parent = wrapping->parent) {
                if (auto* parentSplit = parent->TryGet<Split>()) {
                    Branch* siblingBranch =
                        (parentSplit->left.get() == wrapping)
                            ? parentSplit->right.get()
                            : parentSplit->left.get();
                    if (siblingBranch) {
                        siblingPane = siblingBranch->FindPaneBy(
                            [](Pane const&) { return true; });
                    }
                }
            }
        }
        // Clear the active-pane pointer up front: regardless of which
        // branch runs below, leaving it pointing at the doomed pane
        // would dangle until the SetActivePane calls overwrite it.
        if (closingActive) tab->SetActivePane(nullptr);

        auto result = panelImpl->RemovePane(*pane);
        if (result.IsCollapsed()) {
            // Tab survives; retarget focus to the surviving subtree.
            if (closingActive && siblingPane) {
                tab->SetActivePane(siblingPane);
                if (siblingPane->view) siblingPane->view->TakeFocus();
            }
            return;
        }

        // RemovedRoot or NotFound — treat as full-tab close.
        // (NotFound shouldn't happen, but failing closed by closing
        // the tab is the safer recovery than leaving a half-detached
        // pane around.)
        TearDownTab(*tab);
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
