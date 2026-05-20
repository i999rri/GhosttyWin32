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
                    } else {
                        if (auto* tab = self->ActiveTab()) {
                            tab->Focus();
                        }
                        if (auto* tc = self->ActiveControl()) {
                            tc->NotifyImeFocusEnter();
                        }
                    }
                } catch (winrt::hresult_error const&) {
                }
            });

            auto tv = TabView();
            SetTitleBar(DragRegion());

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
            m_tabFactory = std::make_unique<TabFactory>(m_ghostty->Handle(), m_hwnd, m_paneIds);
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

        auto newLeaf = m_tabFactory->MakeLeaf(newW, newH);
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

        bool activeIsFirst = (node->First() == child);

        auto rect = node->ArrangedRect();
        float extent = (needOrient == SplitOrientation::Horizontal) ? rect.Width : rect.Height;
        float useable = std::max(1.0f,
            extent - static_cast<float>(implementation::SplitPanel::kSplitterThickness));
        double deltaRatio = static_cast<double>(resize.amount) / useable;

        // RIGHT / DOWN push the boundary in the +axis direction.
        // For the first-child side that's an increase in ratio; for
        // the second-child side it's a decrease.
        bool increase = (resize.direction == GHOSTTY_RESIZE_SPLIT_RIGHT
                      || resize.direction == GHOSTTY_RESIZE_SPLIT_DOWN);
        if (!activeIsFirst) increase = !increase;

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
