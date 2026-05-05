#include "pch.h"
#include "MainWindow.xaml.h"
#include "Clipboard.h"
#include "KeyModifiers.h"
#include "Encoding.h"
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

    // SEH "trampoline": isolates a callable invocation behind __try/__except.
    // MSVC's /EHsc refuses __try in any function that has C++ unwinding
    // (i.e. anything dealing with C++ objects). This helper has only raw
    // C types in its frame, so it compiles. The C++ work lives in the
    // callback we invoke through a function pointer — if that callback
    // raises a hardware exception, we swallow it here.
    extern "C" int RunSEHGuarded(void (*fn)(void*), void* ctx) noexcept {
        __try {
            fn(ctx);
            return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            OutputDebugStringA("SEH caught hardware exception inside guarded call\n");
            return 0;
        }
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

            // IME via CoreTextEditContext
            {
                namespace txtCore = winrt::Windows::UI::Text::Core;
                auto manager = txtCore::CoreTextServicesManager::GetForCurrentView();
                m_editContext = manager.CreateEditContext();
                m_editContext.InputPaneDisplayPolicy(txtCore::CoreTextInputPaneDisplayPolicy::Manual);
                m_editContext.InputScope(txtCore::CoreTextInputScope::Default);

                m_editContext.TextRequested([this](txtCore::CoreTextEditContext const&, txtCore::CoreTextTextRequestedEventArgs const& args) {
                    args.Request().Text(winrt::hstring(m_ime.paddedText()));
                });

                m_editContext.SelectionRequested([this](txtCore::CoreTextEditContext const&, txtCore::CoreTextSelectionRequestedEventArgs const& args) {
                    int32_t pos = m_ime.selectionPosition();
                    args.Request().Selection({ pos, pos });
                });

                m_editContext.TextUpdating([this](txtCore::CoreTextEditContext const&, txtCore::CoreTextTextUpdatingEventArgs const& args) {
                    auto range = args.Range();
                    auto newText = args.Text();
                    m_ime.applyTextUpdate(range.StartCaretPosition, range.EndCaretPosition,
                                          newText.c_str(), newText.size());

                    auto* tab = ActiveTab();
                    if (!tab || !tab->Surface()) return;

                    if (m_ime.composing()) {
                        if (m_ime.text().empty()) {
                            ghostty_surface_preedit(tab->Surface(), nullptr, 0);
                        } else {
                            auto utf8 = Encoding::toUtf8(m_ime.text());
                            if (!utf8.empty())
                                ghostty_surface_preedit(tab->Surface(), utf8.c_str(), utf8.size());
                        }
                    }
                    if (m_ghostty) m_ghostty->Tick();
                    ghostty_surface_refresh(tab->Surface());
                });

                m_editContext.CompositionStarted([this](txtCore::CoreTextEditContext const&, txtCore::CoreTextCompositionStartedEventArgs const&) {
                    m_ime.compositionStarted();
                });

                m_editContext.CompositionCompleted([this](txtCore::CoreTextEditContext const&, txtCore::CoreTextCompositionCompletedEventArgs const&) {
                    auto* tab = ActiveTab();
                    if (tab && tab->Surface()) {
                        ghostty_surface_preedit(tab->Surface(), nullptr, 0);
                        auto utf8 = Encoding::toUtf8(m_ime.text());
                        if (!utf8.empty()) {
                            ghostty_surface_text(tab->Surface(), utf8.c_str(), utf8.size());
                        }
                        if (m_ghostty) m_ghostty->Tick();
                        ghostty_surface_refresh(tab->Surface());
                    }
                    m_ime.compositionCompleted();
                });

                m_editContext.LayoutRequested([this](txtCore::CoreTextEditContext const&, txtCore::CoreTextLayoutRequestedEventArgs const& args) {
                    auto* tab = ActiveTab();
                    if (!tab || !tab->Surface() || !m_hwnd) return;
                    double x = 0, y = 0, w = 0, h = 0;
                    ghostty_surface_ime_point(tab->Surface(), &x, &y, &w, &h);
                    POINT screenPt = { (LONG)x, (LONG)y };
                    ClientToScreen(m_hwnd, &screenPt);
                    winrt::Windows::Foundation::Rect bounds{
                        (float)screenPt.x, (float)screenPt.y, (float)w, (float)h };
                    args.Request().LayoutBounds().ControlBounds(bounds);
                    args.Request().LayoutBounds().TextBounds(bounds);
                });

                m_editContext.FocusRemoved([this](txtCore::CoreTextEditContext const&, auto&&) {
                    if (m_ime.composing()) {
                        m_ime.reset();
                        auto* tab = ActiveTab();
                        if (tab && tab->Surface())
                            ghostty_surface_preedit(tab->Surface(), nullptr, 0);
                    }
                });
            }

            // Re-attach IME when window regains activation. The init handler
            // above runs once and calls NotifyFocusEnter only after the first
            // tab is created; without this, switching focus to another window
            // and back leaves CoreTextEditContext detached, so IME stays off
            // even if the OS-level IME toggle is on.
            Activated([this](winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::WindowActivatedEventArgs const& args) {
                if (!m_editContext) return;
                using State = winrt::Microsoft::UI::Xaml::WindowActivationState;
                if (args.WindowActivationState() == State::Deactivated) {
                    m_editContext.NotifyFocusLeave();
                } else if (ActiveTab()) {
                    m_editContext.NotifyFocusEnter();
                }
            });

            auto tv = TabView();
            SetTitleBar(DragRegion());

            // Window-level input handling (same approach as Windows Terminal)
            auto root = Content().as<winrt::Microsoft::UI::Xaml::UIElement>();

            root.KeyDown([this](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                auto* tab = ActiveTab();
                if (!tab || !tab->Surface()) return;

                int vk = static_cast<int>(args.Key());
                UINT scanCode = args.KeyStatus().ScanCode;
                bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
                bool shift = GetKeyState(VK_SHIFT) & 0x8000;

                // IME is processing this key
                if (vk == VK_PROCESSKEY || m_ime.composing()) return;

                // Ctrl+C: copy if selection exists, otherwise send SIGINT
                if (ctrl && !shift && vk == 'C') {
                    if (ghostty_surface_has_selection(tab->Surface())) {
                        ghostty_text_s text = {};
                        if (ghostty_surface_read_selection(tab->Surface(), &text) && text.text && text.text_len > 0) {
                            Clipboard::write(m_hwnd, Encoding::toUtf16(text.text, static_cast<int>(text.text_len)));
                            ghostty_surface_free_text(tab->Surface(), &text);
                        }
                        ghostty_surface_mouse_button(tab->Surface(), GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                        ghostty_surface_mouse_button(tab->Surface(), GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                        args.Handled(true);
                        return;
                    }
                }

                // Ctrl+V: paste from clipboard
                if (ctrl && !shift && vk == 'V') {
                    auto utf8 = Encoding::toUtf8(Clipboard::read(m_hwnd));
                    if (!utf8.empty()) {
                        ghostty_surface_text(tab->Surface(), utf8.c_str(), utf8.size());
                    }
                    if (m_ghostty) m_ghostty->Tick();
                    ghostty_surface_refresh(tab->Surface());
                    args.Handled(true);
                    return;
                }

                // Compute the unshifted codepoint (VK translated with no
                // modifiers held) so unicode-keyed bindings can match.
                // Without this, Binding.Set.getEvent() in
                // input/Binding.zig:2622 falls through every lookup path
                // for entries like `unicode = 't'` (ctrl+shift+t = new_tab,
                // ctrl+shift+w = close_tab, etc.) and returns null. The
                // physical-keyed bindings (ctrl+tab = next_tab,
                // ctrl+shift+arrow_left = previous_tab) already match via
                // keycode; this fixes the unicode ones.
                //
                // We keep text encoding on the separate ghostty_surface_text
                // path below — passing a `text` field here would
                // double-input because encodeKey also writes utf8 to the
                // pty.
                BYTE plainState[256] = {};
                wchar_t unshiftedChars[4] = {};
                int unshiftedCount = ToUnicode(vk, scanCode, plainState, unshiftedChars, 4, 0);
                // Drain any dead-key state ToUnicode left in plainState so
                // the next real keystroke isn't affected.
                wchar_t drain[4] = {};
                ToUnicode(VK_SPACE, 0x39, plainState, drain, 4, 0);

                // Send key event to ghostty (binding match happens here)
                ghostty_input_key_s keyEvent = {};
                keyEvent.action = GHOSTTY_ACTION_PRESS;
                keyEvent.keycode = scanCode;
                if (args.KeyStatus().IsExtendedKey) keyEvent.keycode |= 0xE000;
                keyEvent.mods = currentMods();
                if (unshiftedCount > 0 && unshiftedChars[0] >= 0x20) {
                    keyEvent.unshifted_codepoint = static_cast<uint32_t>(unshiftedChars[0]);
                }
                bool consumed = ghostty_surface_key(tab->Surface(), keyEvent);

                // Translate to text using ToUnicode (replaces CharacterReceived).
                // Skip when the binding consumed the key — otherwise
                // ctrl+shift+t would type "T" into the pty in addition to
                // opening a new tab.
                if (!consumed) {
                    BYTE kbState[256] = {};
                    GetKeyboardState(kbState);
                    wchar_t chars[4] = {};
                    int charCount = ToUnicode(vk, scanCode, kbState, chars, 4, 0);
                    if (charCount > 0 && chars[0] >= 0x20) {
                        char utf8[16] = {};
                        int len = WideCharToMultiByte(CP_UTF8, 0, chars, charCount, utf8, sizeof(utf8), nullptr, nullptr);
                        if (len > 0) {
                            ghostty_surface_text(tab->Surface(), utf8, len);
                        }
                    }
                }

                if (m_ghostty) m_ghostty->Tick();
                ghostty_surface_refresh(tab->Surface());
                args.Handled(true);
            });

            // Mouse input on root
            root.PointerMoved([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                auto* tab = ActiveTab();
                if (!tab || !tab->Surface()) return;
                winrt::Microsoft::UI::Input::PointerPoint point = args.GetCurrentPoint(tab->Panel());
                winrt::Windows::Foundation::Point pos = point.Position();
                ghostty_surface_mouse_pos(tab->Surface(), pos.X, pos.Y, currentMods());
            });

            root.PointerPressed([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                auto* tab = ActiveTab();
                if (!tab || !tab->Surface()) return;
                winrt::Microsoft::UI::Input::PointerPoint point = args.GetCurrentPoint(tab->Panel());
                winrt::Microsoft::UI::Input::PointerPointProperties props = point.Properties();
                ghostty_input_mouse_button_e btn;
                if (props.IsLeftButtonPressed()) btn = GHOSTTY_MOUSE_LEFT;
                else if (props.IsRightButtonPressed()) {
                    // Right-click: copy selection if exists
                    if (ghostty_surface_has_selection(tab->Surface())) {
                        ghostty_text_s text = {};
                        if (ghostty_surface_read_selection(tab->Surface(), &text) && text.text && text.text_len > 0) {
                            Clipboard::write(m_hwnd, Encoding::toUtf16(text.text, static_cast<int>(text.text_len)));
                            ghostty_surface_free_text(tab->Surface(), &text);
                        }
                        ghostty_surface_mouse_button(tab->Surface(), GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                        ghostty_surface_mouse_button(tab->Surface(), GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                        return;
                    }
                    btn = GHOSTTY_MOUSE_RIGHT;
                }
                else return; // Ignore middle-click and others
                ghostty_surface_mouse_button(tab->Surface(), GHOSTTY_MOUSE_PRESS, btn, currentMods());
            });

            root.PointerReleased([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {
                auto* tab = ActiveTab();
                if (!tab || !tab->Surface()) return;
                ghostty_surface_mouse_button(tab->Surface(), GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, currentMods());
            });

            root.PointerWheelChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                auto* tab = ActiveTab();
                if (!tab || !tab->Surface()) return;
                winrt::Microsoft::UI::Input::PointerPoint point = args.GetCurrentPoint(tab->Panel());
                winrt::Microsoft::UI::Input::PointerPointProperties props = point.Properties();
                int delta = props.MouseWheelDelta();
                double scrollY = (double)delta / 120.0;
                ghostty_input_scroll_mods_t smods = {};
                ghostty_surface_mouse_scroll(tab->Surface(), 0, scrollY, smods);
                args.Handled(true);
            });

            root.KeyUp([this](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                auto* tab = ActiveTab();
                if (!tab || !tab->Surface()) return;
                ghostty_input_key_s keyEvent = {};
                keyEvent.action = GHOSTTY_ACTION_RELEASE;
                keyEvent.keycode = args.KeyStatus().ScanCode;
                if (args.KeyStatus().IsExtendedKey) keyEvent.keycode |= 0xE000;
                keyEvent.mods = currentMods();
                ghostty_surface_key(tab->Surface(), keyEvent);
            });

            // DPI change handling (deferred until XamlRoot is available)
            Content().as<winrt::Microsoft::UI::Xaml::FrameworkElement>().Loaded([this](auto&&, auto&&) {
                Content().XamlRoot().Changed([this](auto&&, winrt::Microsoft::UI::Xaml::XamlRootChangedEventArgs const&) {
                    if (!m_hwnd) return;
                    UINT dpi = GetDpiForWindow(m_hwnd);
                    double scale = (double)dpi / 96.0;
                    for (auto& t : m_tabs) {
                        if (t->Surface()) {
                            ghostty_surface_set_content_scale(t->Surface(), scale, scale);
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

            tv.TabCloseRequested([this](muxc::TabView const& sender, muxc::TabViewTabCloseRequestedEventArgs const& args) {
                auto item = args.Tab();
                uint32_t idx = 0;
                if (sender.TabItems().IndexOf(item, idx)) {
                    sender.TabItems().RemoveAt(idx);
                }
                DwmFlush();              // wait for compositor to release
                m_tabs.Remove(item);     // Tab destructor handles teardown
                if (sender.TabItems().Size() == 0) {
                    this->Close();
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
                HANDLE h = tab->SurfaceHandle();
                if (h) CloseHandle(h);
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
                        // Mirror the TabCloseRequested handler: pull the
                        // TabViewItem out of the TabView, DwmFlush so the
                        // compositor releases its reference, then drop the
                        // owning Tab. The Tab destructor frees the ghostty
                        // surface and closes the composition handle.
                        auto* t = mw->m_tabs.FindBySurface(surface);
                        if (!t) return;
                        auto item = t->Item();
                        auto tv = mw->TabView();
                        uint32_t idx = 0;
                        if (tv.TabItems().IndexOf(item, idx)) {
                            tv.TabItems().RemoveAt(idx);
                        }
                        DwmFlush();
                        mw->m_tabs.Remove(item);
                        if (tv.TabItems().Size() == 0) {
                            mw->Close();
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
            auto* tab = g_mainWindow->ActiveTab();
            if (!tab || !tab->Surface()) return false;
            auto utf8 = Encoding::toUtf8(Clipboard::read(g_mainWindow->m_hwnd));
            if (utf8.empty()) return false;
            ghostty_surface_complete_clipboard_request(tab->Surface(), utf8.c_str(), state, false);
            return true;
        };
        rtConfig.confirm_read_clipboard_cb = [](void*, const char* content, void* state, ghostty_clipboard_request_e) {
            // Auto-confirm clipboard reads
            if (g_mainWindow) {
                auto* tab = g_mainWindow->ActiveTab();
                if (tab && tab->Surface()) {
                    ghostty_surface_complete_clipboard_request(tab->Surface(), content, state, true);
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
                auto tv = mw->TabView();
                uint32_t idx = 0;
                if (tv.TabItems().IndexOf(item, idx)) {
                    tv.TabItems().RemoveAt(idx);
                }
                DwmFlush();
                mw->m_tabs.Remove(item);
                if (tv.TabItems().Size() == 0) {
                    mw->Close();
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

        auto panel = muxc::SwapChainPanel();
        panel.IsTabStop(true);
        panel.IsHitTestVisible(true);
        panel.AllowFocusOnInteraction(true);

        auto item = muxc::TabViewItem();
        static constexpr wchar_t kDefaultTabTitle[] = L" ";
        item.Header(box_value(kDefaultTabTitle));
        item.IsClosable(true);
        item.Content(panel);
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
            tvStrong.SelectedItem(itemStrong);
            if (auto* tab = self->m_tabs.FindByItem(itemStrong)) {
                tab->Focus();
            }
            if (self->m_editContext) {
                self->m_ime.reset();
                self->m_editContext.NotifyFocusEnter();
            }
            if (self->m_hwnd) ShowWindow(self->m_hwnd, SW_SHOW);
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
        if (auto* prev = ActiveTab()) {
            auto const& prevPanel = prev->Panel();
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
            muxc::SwapChainPanel const* panel;
            muxc::TabViewItem const* item;
            TabFactory* factory;
            std::function<void()> onActivated;
            uint32_t initialWidth;
            uint32_t initialHeight;
            std::unique_ptr<Tab> result;
        };
        CreateCtx ctx{ &panel, &item, m_tabFactory.get(), std::move(onActivated), initialW, initialH, nullptr };
        int ok = RunSEHGuarded([](void* arg) noexcept {
            auto* c = static_cast<CreateCtx*>(arg);
            c->result = c->factory->Make(*c->panel, *c->item, std::move(c->onActivated), c->initialWidth, c->initialHeight);
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

        // Focus / SW_SHOW / SelectedItem / NotifyFocusEnter are deferred
        // to the onActivated callback fired from Tab once ghostty has
        // presented its first frame.
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
