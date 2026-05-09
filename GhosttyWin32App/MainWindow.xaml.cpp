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
#include <filesystem>
#include <fstream>
#pragma comment(lib, "dwmapi.lib")

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
                    if (auto* tc = t->ActiveControl()) {
                        tc->Detach();
                    }
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
                        if (auto* tc = t->ActiveControl()) {
                            tc->Detach();
                        }
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
        // the surface for any other reason. The userdata is the Tab ID
        // we set in TabFactory::Make. Dispatch the TabView mutation to
        // the next UI tick to mirror the GHOSTTY_ACTION_CLOSE_TAB handler.
        rtConfig.close_surface_cb = [](void* userdata, bool /*process_alive*/) {
            if (!g_mainWindow || !userdata) return;
            TabId id = TabId::FromUserdata(userdata);
            auto mw = g_mainWindow;
            mw->DispatcherQueue().TryEnqueue([mw, id]() {
                auto* t = mw->m_tabs.FindById(id);
                if (!t) return; // Tab already closed via the UI
                auto item = t->Item();
                // Same Detach-before-RemoveAt pattern as the other
                // close paths — see TabCloseRequested.
                if (auto* tc = t->ActiveControl()) {
                    tc->Detach();
                }
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
        };

        m_ghostty = GhosttyApp::Create(rtConfig);
        if (m_ghostty && m_hwnd) {
            m_tabFactory = std::make_unique<TabFactory>(m_ghostty->Handle(), m_hwnd, m_tabIds);
        }
    }

    void MainWindow::CreateTab()
    {
        if (!m_ghostty || !m_hwnd) return;
        auto tv = TabView();

        // Each tab is a TerminalControl (UserControl wrapping a
        // SwapChainPanel). Focus/IsTabStop/etc. are set in the XAML
        // template, so no per-instance setup is needed here.
        auto control = winrt::GhosttyWin32::TerminalControl();

        auto item = muxc::TabViewItem();
        static constexpr wchar_t kDefaultTabTitle[] = L" ";
        item.Header(box_value(kDefaultTabTitle));
        item.IsClosable(true);
        item.Content(control);
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
            winrt::GhosttyWin32::TerminalControl const* control;
            muxc::TabViewItem const* item;
            TabFactory* factory;
            std::function<void()> onActivated;
            uint32_t initialWidth;
            uint32_t initialHeight;
            std::unique_ptr<Tab> result;
        };
        CreateCtx ctx{ &control, &item, m_tabFactory.get(), std::move(onActivated), initialW, initialH, nullptr };
        int ok = RunSEHGuarded([](void* arg) noexcept {
            auto* c = static_cast<CreateCtx*>(arg);
            c->result = c->factory->Make(*c->control, *c->item, std::move(c->onActivated), c->initialWidth, c->initialHeight);
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
