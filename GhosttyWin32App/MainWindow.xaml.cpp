#include "pch.h"
#include "MainWindow.xaml.h"
#include "Clipboard.h"
#include "KeyModifiers.h"
#include "Encoding.h"
#include "SEHGuard.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include <microsoft.ui.xaml.window.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl_core.h>
#include <winrt/Microsoft.Windows.AppNotifications.h>
#include <winrt/Microsoft.Windows.AppNotifications.Builder.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

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
            SetTitleBar(DragRegion());

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

            // DPI change handling (deferred until XamlRoot is available)
            Content().as<winrt::Microsoft::UI::Xaml::FrameworkElement>().Loaded([this](auto&&, auto&&) {
                Content().XamlRoot().Changed([this](auto&&, winrt::Microsoft::UI::Xaml::XamlRootChangedEventArgs const&) {
                    if (!m_hwnd) return;
                    UINT dpi = GetDpiForWindow(m_hwnd);
                    double scale = (double)dpi / 96.0;
                    // Today every tab has a single TerminalControl. With
                    // future pane support this would walk each tab's
                    // pane tree and apply the scale to every leaf.
                    for (auto& t : m_tabs) {
                        if (auto* tc = t->ActiveControl(); tc && tc->Surface()) {
                            ghostty_surface_set_content_scale(tc->Surface(), scale, scale);
                        }
                    }
                });
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
                    // destructors run.
                    this->Close();
                } else {
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
                    if (auto* tab = self->ActiveTab()) {
                        tab->Focus();
                    }
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

    void MainWindow::NotifySurfaceFocused(ghostty_surface_t surface) noexcept
    {
        m_activeSurface = surface;
    }

    void MainWindow::InitGhostty()
    {
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
            // Tab lifecycle / navigation actions. ghostty's default keybinds
            // (Ctrl+Shift+T new tab, Ctrl+Shift+W close, Ctrl+Tab/Ctrl+PageDown
            // next, etc.) are matched on the renderer thread inside
            // ghostty_surface_key and surfaced here as actions; the actual
            // TabView mutation has to happen on the UI thread.
            //
            // NEW_WINDOW is folded into NEW_TAB for now since multi-window
            // isn't implemented — this matches how other shells fall back
            // when they get a "new window" request without a window manager.
            if (action.tag == GHOSTTY_ACTION_NEW_TAB ||
                action.tag == GHOSTTY_ACTION_NEW_WINDOW) {
                if (g_mainWindow) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw]() {
                        if (g_mainWindow) g_mainWindow->CreateTab();
                    });
                }
                return true;
            }

            if (action.tag == GHOSTTY_ACTION_CLOSE_TAB &&
                target.tag == GHOSTTY_TARGET_SURFACE) {
                auto surface = target.target.surface;
                if (g_mainWindow && surface) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw, surface]() {
                        // Mirror the TabCloseRequested handler — see
                        // there for why Detach runs before RemoveAt and
                        // why the last tab's Tab destruction is deferred
                        // to ~MainWindow.
                        auto* t = mw->m_tabs.FindBySurface(surface);
                        if (!t) return;
                        auto item = t->Item();
                        // CLOSE_TAB closes the whole tab regardless of
                        // pane count — detach every leaf so each swap
                        // chain handle is cleared before unparent.
                        t->DetachAll();
                        auto tv = mw->TabView();
                        uint32_t idx = 0;
                        if (tv.TabItems().IndexOf(item, idx)) {
                            tv.TabItems().RemoveAt(idx);
                        }
                        DwmFlush();
                        if (tv.TabItems().Size() == 0) {
                            mw->Close();
                        } else {
                            mw->m_tabs.Remove(item);
                        }
                    });
                }
                return true;
            }

            if (action.tag == GHOSTTY_ACTION_GOTO_TAB) {
                int requested = static_cast<int>(action.action.goto_tab);
                if (g_mainWindow) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw, requested]() {
                        auto tv = mw->TabView();
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
                    });
                }
                return true;
            }

            if ((action.tag == GHOSTTY_ACTION_SET_TITLE || action.tag == GHOSTTY_ACTION_SET_TAB_TITLE)
                && target.tag == GHOSTTY_TARGET_SURFACE) {
                const char* title = action.action.set_title.title;
                auto surface = target.target.surface;
                if (title && g_mainWindow) {
                    auto wstr = std::make_shared<std::wstring>(Encoding::toUtf16(title));
                    if (!wstr->empty()) {
                        auto mw = g_mainWindow;
                        mw->DispatcherQueue().TryEnqueue([mw, wstr, surface]() {
                            if (auto* t = mw->m_tabs.FindBySurface(surface)) {
                                t->Item().Header(box_value(winrt::hstring(*wstr)));
                            }
                        });
                    }
                }
            }

            // Title bar and tab strip color matches terminal background
            if (action.tag == GHOSTTY_ACTION_COLOR_CHANGE && g_mainWindow && g_mainWindow->m_hwnd) {
                auto& cc = action.action.color_change;
                if (cc.kind == GHOSTTY_ACTION_COLOR_KIND_BACKGROUND) {
                    HWND hwnd = g_mainWindow->m_hwnd;
                    COLORREF color = RGB(cc.r, cc.g, cc.b);
                    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &color, sizeof(color));
                    float luminance = 0.299f * cc.r + 0.587f * cc.g + 0.114f * cc.b;
                    COLORREF textColor = (luminance < 128) ? RGB(255, 255, 255) : RGB(0, 0, 0);
                    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));

                    // Update XAML background to match
                    auto mw = g_mainWindow;
                    uint8_t r = cc.r, g = cc.g, b = cc.b;
                    mw->DispatcherQueue().TryEnqueue([mw, r, g, b]() {
                        auto brush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
                            winrt::Windows::UI::Color{ 255, r, g, b });
                        mw->Content().as<winrt::Microsoft::UI::Xaml::Controls::Panel>().Background(brush);
                    });
                }
                return true;
            }

            // Pointer cursor shape requests — IBeam over cells, Hand
            // when Ctrl-hovering a link, resize arrows on split borders
            // (not yet exposed), etc. Per-surface so multi-tab windows
            // can have different cursors per tab.
            if (action.tag == GHOSTTY_ACTION_MOUSE_SHAPE
                && target.tag == GHOSTTY_TARGET_SURFACE) {
                auto surface = target.target.surface;
                auto shape = action.action.mouse_shape;
                if (g_mainWindow && surface) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw, surface, shape]() {
                        if (auto* t = mw->m_tabs.FindBySurface(surface)) {
                            if (auto* tc = t->ActiveControl()) {
                                tc->SetCursorShape(shape);
                            }
                        }
                    });
                }
                return true;
            }

            // Zoom the source pane to fill the entire tab. A second
            // press unzooms back to the regular split layout.
            if (action.tag == GHOSTTY_ACTION_TOGGLE_SPLIT_ZOOM
                && target.tag == GHOSTTY_TARGET_SURFACE) {
                auto surface = target.target.surface;
                if (g_mainWindow && surface) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw, surface]() {
                        mw->ToggleSplitZoomForSurface(surface);
                    });
                }
                return true;
            }

            // Reset all split ratios in the source tab to 0.5 so each
            // pane gets an even share of its parent split.
            if (action.tag == GHOSTTY_ACTION_EQUALIZE_SPLITS
                && target.tag == GHOSTTY_TARGET_SURFACE) {
                auto surface = target.target.surface;
                if (g_mainWindow && surface) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw, surface]() {
                        mw->EqualizeSplitsForSurface(surface);
                    });
                }
                return true;
            }

            // Move focus to another pane within the same tab — the
            // direction variants walk the tree by arranged-rect
            // adjacency, the sequential variants cycle DFS order.
            if (action.tag == GHOSTTY_ACTION_GOTO_SPLIT
                && target.tag == GHOSTTY_TARGET_SURFACE) {
                auto surface = target.target.surface;
                auto direction = action.action.goto_split;
                if (g_mainWindow && surface) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw, surface, direction]() {
                        mw->GotoSplitFromAction(surface, direction);
                    });
                }
                return true;
            }

            // Keyboard-driven split resize. Same underlying ratio
            // mutation as the splitter-drag path, just initiated from
            // a ghostty keybind instead of pointer drag. `amount` is
            // treated as DIPs along the split axis.
            if (action.tag == GHOSTTY_ACTION_RESIZE_SPLIT
                && target.tag == GHOSTTY_TARGET_SURFACE) {
                auto surface = target.target.surface;
                auto resize = action.action.resize_split;
                if (g_mainWindow && surface) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw, surface, resize]() {
                        mw->ResizeSplitFromAction(surface, resize);
                    });
                }
                return true;
            }

            // Split the source pane along the requested direction. The
            // existing pane stays put and a new TerminalControl /
            // ghostty surface is inserted alongside it; the active
            // leaf shifts to the new pane so the user's next keystroke
            // lands in the split they just created.
            if (action.tag == GHOSTTY_ACTION_NEW_SPLIT
                && target.tag == GHOSTTY_TARGET_SURFACE) {
                auto surface = target.target.surface;
                auto direction = action.action.new_split;
                if (g_mainWindow && surface) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw, surface, direction]() {
                        mw->SplitActivePane(surface, direction);
                    });
                }
                return true;
            }

            // Terminal sent BEL (\x07). MessageBeep is the obvious
            // Windows-native equivalent — it plays whatever the user
            // has bound to the "Default Beep" system sound, runs
            // asynchronously, and is thread-safe (so we don't need
            // to bounce through the dispatcher queue). Honouring the
            // ghostty `bell-features` config (audio / attention /
            // title / unread) is a follow-up; this gets the audible
            // path working.
            if (action.tag == GHOSTTY_ACTION_RING_BELL) {
                MessageBeep(MB_OK);
                return true;
            }

            // Single-window builds collapse CLOSE_WINDOW, QUIT, and
            // CLOSE_ALL_WINDOWS into the same effect — close the one
            // window we have, which terminates the app. Multi-window
            // support (#55) will need to give these three distinct
            // behaviours.
            if (action.tag == GHOSTTY_ACTION_CLOSE_WINDOW
                || action.tag == GHOSTTY_ACTION_CLOSE_ALL_WINDOWS
                || action.tag == GHOSTTY_ACTION_QUIT) {
                if (g_mainWindow) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw]() {
                        try { mw->Close(); }
                        catch (winrt::hresult_error const&) {}
                    });
                }
                return true;
            }

            // Copy the active tab/surface title to the system
            // clipboard. The title lives on the TabViewItem.Header
            // (set by SET_TITLE / SET_TAB_TITLE earlier), so we
            // unbox it back to hstring and round-trip it through
            // the same Clipboard::write the selection-copy path
            // uses.
            if (action.tag == GHOSTTY_ACTION_COPY_TITLE_TO_CLIPBOARD
                && target.tag == GHOSTTY_TARGET_SURFACE) {
                auto surface = target.target.surface;
                if (g_mainWindow && surface) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw, surface]() {
                        auto* t = mw->m_tabs.FindBySurface(surface);
                        if (!t) return;
                        auto title = winrt::unbox_value_or<winrt::hstring>(
                            t->Item().Header(), winrt::hstring{});
                        if (title.empty()) return;
                        Clipboard::write(mw->m_hwnd, std::wstring(title));
                    });
                }
                return true;
            }

            // Open the user's ghostty config in their default editor.
            // The Windows config path is %LOCALAPPDATA%\ghostty\config
            // (no extension); if the user has no association for
            // extension-less files Windows shows the "Open With"
            // dialog, which is the right OS-native behaviour for
            // first run.
            //
            // GetEnvironmentVariableW over _wgetenv: the CRT helper
            // is marked deprecated under MSVC /W4, the Win32 API is
            // the documented modern path and writes into a caller-
            // supplied buffer so there's no heap-allocation cleanup.
            if (action.tag == GHOSTTY_ACTION_OPEN_CONFIG) {
                wchar_t appdata[MAX_PATH];
                DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", appdata,
                                                   static_cast<DWORD>(std::size(appdata)));
                if (len > 0 && len < std::size(appdata)) {
                    std::wstring path = std::wstring(appdata) + L"\\ghostty\\config";
                    ShellExecuteW(nullptr, L"open", path.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
                return true;
            }

            // Toggle minimize / restore. We use SW_MINIMIZE /
            // SW_RESTORE instead of SW_HIDE here because hiding the
            // window from the taskbar leaves Windows users without a
            // discoverable way back — ghostty's `global:` keybind
            // qualifier isn't wired to RegisterHotKey on this port
            // yet, so a SW_HIDE'd window with no taskbar entry can
            // only be recovered by relaunching. Minimizing keeps
            // the window reachable via taskbar click / alt-tab,
            // which matches what Windows users expect from a
            // "toggle visibility" bind. The Mac-style full hide
            // semantics can come back once global hotkeys land.
            if (action.tag == GHOSTTY_ACTION_TOGGLE_VISIBILITY) {
                if (g_mainWindow) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw]() {
                        HWND hwnd = mw->m_hwnd;
                        if (!hwnd) return;
                        if (IsIconic(hwnd)) {
                            ShowWindow(hwnd, SW_RESTORE);
                            SetForegroundWindow(hwnd);
                        } else {
                            ShowWindow(hwnd, SW_MINIMIZE);
                        }
                    });
                }
                return true;
            }

            // FLOAT_WINDOW (always-on-top toggle) — DISABLED.
            // No keybind we tried reached this branch: ctrl+shift+f
            // hits the ghostty-default start_search, ctrl+shift+alt+f
            // is swallowed by the WinUI Alt-menu accelerator before
            // ghostty sees it, and ctrl+shift+backslash produced no
            // observable action_cb call either. The Win32 side of the
            // implementation (SetWindowPos with HWND_TOPMOST /
            // HWND_NOTOPMOST) is straightforward and preserved below
            // for when the dispatch path is understood; the gate is
            // figuring out why ghostty isn't dispatching the action
            // to action_cb. Re-enable after that is resolved.
#if 0
            if (action.tag == GHOSTTY_ACTION_FLOAT_WINDOW) {
                auto mode = action.action.float_window;
                if (g_mainWindow) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw, mode]() {
                        HWND hwnd = mw->m_hwnd;
                        if (!hwnd) return;
                        bool wantTop;
                        switch (mode) {
                            case GHOSTTY_FLOAT_WINDOW_ON:  wantTop = true; break;
                            case GHOSTTY_FLOAT_WINDOW_OFF: wantTop = false; break;
                            case GHOSTTY_FLOAT_WINDOW_TOGGLE:
                            default:
                                wantTop = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0;
                                break;
                        }
                        SetWindowPos(hwnd, wantTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                                     0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    });
                }
                return true;
            }
#endif

            // Reload the configuration. ghostty fires RELOAD_CONFIG
            // when the user hits the default ctrl+shift+, keybind
            // (or when a soft reload is requested internally, e.g.
            // after a CONFIG_CHANGE notification). For soft reloads
            // we just re-apply the config we already hold; for hard
            // reloads we re-parse the file on a 4MB-stack worker
            // thread (same reason GhosttyApp::Create uses one — the
            // config parser stack-overflows the default 1MB) and
            // hand the result to the UI thread for swap, since
            // that's where ghostty_app_tick lives.
            if (action.tag == GHOSTTY_ACTION_RELOAD_CONFIG) {
                bool soft = action.action.reload_config.soft;
                if (!g_mainWindow || !g_mainWindow->m_ghostty) return true;
                auto mw = g_mainWindow;

                if (soft) {
                    mw->DispatcherQueue().TryEnqueue([mw]() {
                        if (!mw->m_ghostty) return;
                        auto app = mw->m_ghostty->Handle();
                        auto cfg = mw->m_ghostty->ConfigHandle();
                        if (app && cfg) ghostty_app_update_config(app, cfg);
                    });
                    return true;
                }

                struct ReloadCtx { MainWindow* mw; };
                auto* ctx = new ReloadCtx{ mw };
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
                return true;
            }

            // Toggle maximize/restore via the same WM_SYSCOMMAND
            // path the caption-button click already uses, so the
            // NVIDIA OverlappedPresenter AV from issue #26 stays out
            // of the picture. SendMessage runs on the UI thread;
            // dispatch to it because action_cb fires from the
            // renderer thread.
            if (action.tag == GHOSTTY_ACTION_TOGGLE_MAXIMIZE) {
                if (g_mainWindow) {
                    auto mw = g_mainWindow;
                    mw->DispatcherQueue().TryEnqueue([mw]() {
                        HWND hwnd = mw->m_hwnd;
                        if (!hwnd) return;
                        SendMessageW(hwnd, WM_SYSCOMMAND,
                                     IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
                    });
                }
                return true;
            }

            // Quit-timer ack. macOS apprts use this to manage the
            // "wait N seconds after the last window closes before
            // actually quitting" countdown — Cmd+Q behavior carries
            // through that on macOS. Windows quits as soon as the
            // last top-level HWND goes away (which our CLOSE_WINDOW
            // path already does), so neither START nor STOP needs
            // any wiring. Return true so future libghostty versions
            // don't start logging "unhandled action" for an action
            // we've intentionally ignored.
            if (action.tag == GHOSTTY_ACTION_QUIT_TIMER) {
                return true;
            }

            // DESKTOP_NOTIFICATION — surface ghostty's bell-and-toast
            // notifications via the Windows native toast layer. Builds
            // a minimal two-line payload (title + body) and hands it
            // to AppNotificationManager; the manager was registered
            // in App::OnLaunched so Show won't be silently dropped.
            // Dispatch to the UI thread because the AppNotifications
            // WinRT projection is happiest on the STA-initialised UI
            // thread and we already cross that boundary for every
            // other Win32 surface call.
            if (action.tag == GHOSTTY_ACTION_DESKTOP_NOTIFICATION) {
                auto& dn = action.action.desktop_notification;
                std::wstring title = (dn.title && dn.title[0]) ? Encoding::toUtf16(dn.title) : L"";
                std::wstring body  = (dn.body  && dn.body[0])  ? Encoding::toUtf16(dn.body)  : L"";
                if (title.empty() && body.empty()) return true;
                if (!g_mainWindow) return true;
                auto mw = g_mainWindow;
                mw->DispatcherQueue().TryEnqueue([title = std::move(title),
                                                  body = std::move(body)]() {
                    try {
                        using namespace winrt::Microsoft::Windows::AppNotifications;
                        using namespace winrt::Microsoft::Windows::AppNotifications::Builder;
                        AppNotificationBuilder builder;
                        if (!title.empty()) builder.AddText(title);
                        if (!body.empty())  builder.AddText(body);
                        AppNotificationManager::Default().Show(builder.BuildNotification());
                    } catch (winrt::hresult_error const&) {
                        // Either the manager wasn't registered (App startup
                        // logged it) or the OS refused. Nothing actionable
                        // host-side; the message just doesn't appear.
                    }
                });
                return true;
            }

            // PROGRESS_REPORT — OSC 9;4 progress reports from the
            // shell (pnpm, make, large copies, etc.). Without a host
            // handler the percentage just gets dropped on the floor.
            // Map ghostty's state machine onto ITaskbarList3 so the
            // user sees the progress bar on the taskbar button — the
            // standard Windows surface for background-task progress.
            // The ITaskbarList3 instance is cached on the UI thread
            // (where COM is STA-initialized) since CoCreateInstance +
            // HrInit aren't cheap to redo per OSC sequence.
            if (action.tag == GHOSTTY_ACTION_PROGRESS_REPORT) {
                auto pr = action.action.progress_report;
                if (!g_mainWindow) return true;
                auto mw = g_mainWindow;
                mw->DispatcherQueue().TryEnqueue([mw, pr]() {
                    HWND hwnd = mw->m_hwnd;
                    if (!hwnd) return;
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
                    s_taskbar->SetProgressState(hwnd, flag);
                    // SetProgressValue is meaningless under
                    // INDETERMINATE / NOPROGRESS and the percentage
                    // is -1 when no value was reported — skip the
                    // call so the bar doesn't snap to 0% on a
                    // bare state change.
                    if (pr.progress >= 0
                        && (flag == TBPF_NORMAL || flag == TBPF_ERROR || flag == TBPF_PAUSED)) {
                        s_taskbar->SetProgressValue(hwnd,
                                                    static_cast<ULONGLONG>(pr.progress),
                                                    100ULL);
                    }
                });
                return true;
            }

            // Acknowledge informational actions we don't have UI for
            // yet. Each of these would deserve its own surface-side
            // element in a fully-featured port — a read-only banner,
            // a secure-input padlock, a pending-chord indicator, a
            // modal-key-table label, a shell-supplied title source
            // flag, a PWD breadcrumb, a post-command summary — but
            // none of that UI exists today, and letting the action
            // fall through to action_cb's unhandled-default branch
            // would leave the door open to libghostty logging it as
            // missing in a future audit. Returning true keeps that
            // signal quiet; the proper UI work is tracked in #57.
            if (action.tag == GHOSTTY_ACTION_READONLY
                || action.tag == GHOSTTY_ACTION_SECURE_INPUT
                || action.tag == GHOSTTY_ACTION_KEY_SEQUENCE
                || action.tag == GHOSTTY_ACTION_KEY_TABLE
                || action.tag == GHOSTTY_ACTION_PROMPT_TITLE
                || action.tag == GHOSTTY_ACTION_PWD
                || action.tag == GHOSTTY_ACTION_COMMAND_FINISHED) {
                return true;
            }

            // CHECK_FOR_UPDATES — ghostty has no built-in updater on
            // Windows; the action just lets the host decide what
            // "check for updates" means. Sending the user to the
            // GitHub releases page is the lowest-friction option
            // while we don't ship a Sparkle-equivalent updater.
            // ShellExecuteW dispatches to the user's default browser
            // without spawning a visible cmd/rundll32 flash, matching
            // the OPEN_URL path.
            if (action.tag == GHOSTTY_ACTION_CHECK_FOR_UPDATES) {
                HWND hwnd = g_mainWindow ? g_mainWindow->m_hwnd : nullptr;
                ShellExecuteW(hwnd, L"open",
                              L"https://github.com/i999rri/GhosttyWin32/releases",
                              nullptr, nullptr, SW_SHOWNORMAL);
                return true;
            }

            // SHOW_CHILD_EXITED — ghostty notifies us the surface's
            // shell process exited. With confirm-close-surface=false
            // (our default) the surface tears itself down via
            // close_surface_cb almost immediately, so this is mostly
            // a diagnostic breadcrumb: when a shell crashes during
            // development, the exit code in stderr is the fastest
            // path to "what failed". An in-terminal overlay would be
            // the proper UI but needs its own design pass — for now,
            // log and move on.
            if (action.tag == GHOSTTY_ACTION_SHOW_CHILD_EXITED) {
                auto ce = action.action.child_exited;
                std::fprintf(stderr, "[child_exited] exit_code=%u after_ms=%llu\n",
                             ce.exit_code,
                             static_cast<unsigned long long>(ce.timetime_ms));
                std::fflush(stderr);
                return true;
            }

            // RENDER — ghostty is asking for an explicit repaint
            // outside the natural wakeup_cb -> tick cadence. The
            // dispatcher used by wakeup_cb already serialises ticks
            // on the UI thread; route through the same path so the
            // frame lands without us needing a separate per-surface
            // draw entry. ghostty_app_tick is idempotent — calling it
            // when nothing is dirty is a cheap no-op.
            if (action.tag == GHOSTTY_ACTION_RENDER) {
                if (!g_mainWindow || !g_mainWindow->m_ghostty) return true;
                auto mw = g_mainWindow;
                mw->DispatcherQueue().TryEnqueue([mw]() {
                    if (mw->m_ghostty) mw->m_ghostty->Tick();
                });
                return true;
            }

            // SCROLLBAR — ghostty exports scrollback total / offset /
            // visible-len whenever the scroll position moves. We don't
            // render a scrollbar (the terminal surface fills the
            // available area without one), so the data has nowhere to
            // go. Acknowledge anyway so the action doesn't fall
            // through to the unhandled-default branch.
            if (action.tag == GHOSTTY_ACTION_SCROLLBAR) {
                return true;
            }

            // Cache the glyph cell dimensions ghostty derives from the
            // active font + size. ghostty fires this whenever the cell
            // metrics change (font reload, DPI change, config edit);
            // without caching, any future host-side logic that wants
            // to snap a resize / split ratio to a whole-cell boundary
            // would have to round-trip through libghostty each time.
            if (action.tag == GHOSTTY_ACTION_CELL_SIZE) {
                if (!g_mainWindow) return true;
                auto cs = action.action.cell_size;
                g_mainWindow->m_cellWidth = cs.width;
                g_mainWindow->m_cellHeight = cs.height;
                return true;
            }

            // Record the desired startup window dimensions ghostty
            // computes from config (`window-width` × `cell-width-px`,
            // etc.). Stored as physical pixels — ghostty already did
            // the cell-to-pixel math, so RESET_WINDOW_SIZE can hand
            // the value to SetWindowPos directly without re-scaling.
            // Without this the reset target stays the hardcoded
            // 1280x720 fallback below, which ignores the user's
            // config-defined window size.
            if (action.tag == GHOSTTY_ACTION_INITIAL_SIZE) {
                if (!g_mainWindow) return true;
                auto sz = action.action.initial_size;
                g_mainWindow->m_initialWidth = sz.width;
                g_mainWindow->m_initialHeight = sz.height;
                return true;
            }

            // RESET_WINDOW_SIZE — restore the window to its startup
            // footprint. Prefer the size INITIAL_SIZE recorded (which
            // honors the user's config); if INITIAL_SIZE never fired
            // (e.g., default config, no startup size override) fall
            // back to 1280x720 DIPs, which lines up with the WinUI 3
            // fresh-window default and gives an 80-ish column / 24-
            // row terminal at common font sizes.
            if (action.tag == GHOSTTY_ACTION_RESET_WINDOW_SIZE) {
                if (!g_mainWindow) return true;
                auto mw = g_mainWindow;
                mw->DispatcherQueue().TryEnqueue([mw]() {
                    HWND hwnd = mw->m_hwnd;
                    if (!hwnd) return;
                    int width, height;
                    if (mw->m_initialWidth && mw->m_initialHeight) {
                        width = static_cast<int>(mw->m_initialWidth);
                        height = static_cast<int>(mw->m_initialHeight);
                    } else {
                        UINT dpi = GetDpiForWindow(hwnd);
                        width = MulDiv(1280, dpi, 96);
                        height = MulDiv(720, dpi, 96);
                    }
                    SetWindowPos(hwnd, nullptr, 0, 0, width, height,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                });
                return true;
            }

            // CONFIG_CHANGE sync. ghostty has already applied the new
            // config internally by the time this fires; it's a
            // notification, not a request. Without grabbing the new
            // pointer here our stored m_config would diverge from
            // what libghostty is actually using — soft-reload would
            // re-apply the stale copy and any future host-side
            // config query (color theme, padding, etc.) would lie.
            // Clone because ghostty owns the passed-in pointer; the
            // UI-thread swap mirrors the reload_config path so we
            // don't free while another tick could be reading.
            if (action.tag == GHOSTTY_ACTION_CONFIG_CHANGE) {
                auto newCfg = action.action.config_change.config;
                if (!g_mainWindow || !g_mainWindow->m_ghostty || !newCfg) return true;
                auto mw = g_mainWindow;
                auto cloned = ghostty_config_clone(newCfg);
                if (!cloned) return true;
                mw->DispatcherQueue().TryEnqueue([mw, cloned]() {
                    if (!mw->m_ghostty) {
                        ghostty_config_free(cloned);
                        return;
                    }
                    mw->m_ghostty->ReplaceConfig(cloned);
                });
                return true;
            }

            // Auto-hide the cursor while the user is typing — the
            // ghostty convention (matching Vim / Helix / Kitty) is
            // to fire HIDDEN on first keystroke after motion and
            // VISIBLE on the next WM_MOUSEMOVE-equivalent. ShowCursor
            // is a counter, not a boolean, so blindly calling
            // ShowCursor(FALSE) on every HIDDEN would tally the
            // counter into the negative thousands and the matching
            // VISIBLE calls couldn't catch up. Track our own bool
            // and only toggle once per state transition.
            if (action.tag == GHOSTTY_ACTION_MOUSE_VISIBILITY) {
                bool hide = action.action.mouse_visibility == GHOSTTY_MOUSE_HIDDEN;
                if (!g_mainWindow) return true;
                auto mw = g_mainWindow;
                mw->DispatcherQueue().TryEnqueue([hide]() {
                    static bool s_cursorHidden = false;
                    if (hide && !s_cursorHidden) {
                        ShowCursor(FALSE);
                        s_cursorHidden = true;
                    } else if (!hide && s_cursorHidden) {
                        ShowCursor(TRUE);
                        s_cursorHidden = false;
                    }
                });
                return true;
            }

            // Renderer health status from ghostty. UNHEALTHY means the
            // generic renderer detected a problem (texture allocation
            // failure, shader compile fault, etc.) and switched into a
            // degraded mode. Surfacing this as a single stderr line
            // makes the underlying cause findable in the debugger
            // output without committing to a user-facing surface
            // (toast, status bar) just yet — those can layer on top
            // later if we want recovery UX.
            if (action.tag == GHOSTTY_ACTION_RENDERER_HEALTH) {
                bool healthy = action.action.renderer_health == GHOSTTY_RENDERER_HEALTH_HEALTHY;
                std::fprintf(stderr, "[renderer_health] %s\n",
                             healthy ? "healthy" : "unhealthy");
                std::fflush(stderr);
                return true;
            }

            // Ctrl+click on a URL in the terminal. Hand off to the shell
            // verb opener so the user's default browser / mail client /
            // etc. handles it. Without this, libghostty falls back to
            // spawning `rundll32 url.dll,FileProtocolHandler` via
            // std.process.Child, which works but is slower and leaves a
            // brief child-process flash visible in tools like Process
            // Hacker.
            if (action.tag == GHOSTTY_ACTION_OPEN_URL) {
                auto& ou = action.action.open_url;
                if (ou.url && ou.len > 0) {
                    std::wstring wurl = Encoding::toUtf16(ou.url, static_cast<int>(ou.len));
                    if (!wurl.empty()) {
                        HWND hwnd = g_mainWindow ? g_mainWindow->m_hwnd : nullptr;
                        ShellExecuteW(hwnd, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
                return true;
            }

            return false;
        };
        rtConfig.read_clipboard_cb = [](void*, ghostty_clipboard_e, void* state) -> bool {
            if (!g_mainWindow) return false;
            auto* tc = g_mainWindow->ActiveControl();
            if (!tc || !tc->Surface()) return false;
            auto utf8 = Encoding::toUtf8(Clipboard::read(g_mainWindow->m_hwnd));
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
            Clipboard::write(hwnd, Encoding::toUtf16(content[0].data));
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

        m_ghostty = GhosttyApp::Create(rtConfig);
        if (m_ghostty && m_hwnd) {
            // Capture by raw `this`: MainWindow outlives every
            // TerminalControl it owns (the controls are destroyed
            // through Tabs, which is a MainWindow member), so the
            // lambda staying alive on the factory is safe.
            auto onLeafFocused = [this](ghostty_surface_t surface) noexcept {
                NotifySurfaceFocused(surface);
            };
            m_tabFactory = std::make_unique<TabFactory>(
                m_ghostty->Handle(),
                m_ghostty->ConfigHandle(),
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
        // item.Content is set by TabFactory::Make (which wraps the
        // control in a SplitPanel-backed Pane tree). Leaving it unset
        // here keeps "the SplitPanel owns the pane tree" in a single
        // place.
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

        // Estimate the new panel's eventual size from the currently active
        // tab. Both panels live in the same TabView content area, so the
        // active tab's ActualWidth/Height is exactly what the new panel
        // will lay out to once it becomes visible. Passing this lets
        // ghostty create the swap chain at the right size from the start
        // — without it, the new panel's ActualWidth is 0 (deferred
        // SelectedItem) and ghostty falls back to the main window's full
        // client rect, which is too tall by the tab strip height.
        uint32_t initialW = 0, initialH = 0;
        if (auto* prevControl = ActiveControl()) {
            auto prevPanel = prevControl->InnerPanel();
            initialW = static_cast<uint32_t>(prevPanel.ActualWidth());
            initialH = static_cast<uint32_t>(prevPanel.ActualHeight());
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

        // SelectedItem / SW_SHOW are deferred to the onActivated
        // callback fired from Tab once ghostty has presented its first
        // frame; focus + IME activation chain off SelectedItem via the
        // TerminalControl's Loaded → Focus → GotFocus path.
        m_tabs.Add(std::move(tab));
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
        // current SwapChainPanel size halved on the split axis. The
        // SplitPanel's first arrange pass after ReplaceLeaf will
        // re-size both leaves to their actual half-extent and trigger
        // SizeChanged → ghostty resize anyway; this just keeps the
        // initial swap chain close to the eventual size so the first
        // frame doesn't have to stretch.
        uint32_t srcW = 0, srcH = 0;
        if (auto* srcTc = Tab::LeafToTerminalControl(*sourceLeaf)) {
            auto p = srcTc->InnerPanel();
            srcW = static_cast<uint32_t>(p.ActualWidth());
            srcH = static_cast<uint32_t>(p.ActualHeight());
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
