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
                    if (m_app) ghostty_app_tick(m_app);
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
                        if (m_app) ghostty_app_tick(m_app);
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
                    if (m_app) ghostty_app_tick(m_app);
                    ghostty_surface_refresh(tab->Surface());
                    args.Handled(true);
                    return;
                }

                // Send key event to ghostty
                ghostty_input_key_s keyEvent = {};
                keyEvent.action = GHOSTTY_ACTION_PRESS;
                keyEvent.keycode = scanCode;
                if (args.KeyStatus().IsExtendedKey) keyEvent.keycode |= 0xE000;
                keyEvent.mods = currentMods();
                ghostty_surface_key(tab->Surface(), keyEvent);

                // Translate to text using ToUnicode (replaces CharacterReceived)
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

                if (m_app) ghostty_app_tick(m_app);
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
        m_tabs.Clear();  // Tab destructors handle cleanup
        if (m_app) ghostty_app_free(m_app);
        if (m_config) ghostty_config_free(m_config);
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
        struct Args { MainWindow* self; };
        Args args{ this };
        HANDLE hThread = CreateThread(nullptr, 4 * 1024 * 1024,
            [](LPVOID param) -> DWORD {
                auto* a = static_cast<Args*>(param);
                ghostty_init(0, nullptr);
                ghostty_runtime_config_s rtConfig{};
                rtConfig.userdata = a->self;
                rtConfig.wakeup_cb = [](void*) {
                    if (!g_mainWindow || !g_mainWindow->m_app) return;
                    g_mainWindow->DispatcherQueue().TryEnqueue([]() {
                        if (g_mainWindow && g_mainWindow->m_app) {
                            ghostty_app_tick(g_mainWindow->m_app);
                        }
                    });
                };
                rtConfig.action_cb = [](ghostty_app_t, ghostty_target_s target, ghostty_action_s action) -> bool {
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
                // TODO: ghostty doesn't call close_surface_cb on shell exit (see ghostty#34)
                rtConfig.close_surface_cb = [](void*, bool) {};
                a->self->m_config = ghostty_config_new();
                ghostty_config_load_default_files(a->self->m_config);
                ghostty_config_finalize(a->self->m_config);
                a->self->m_app = ghostty_app_new(&rtConfig, a->self->m_config);
                return 0;
            }, &args, 0, nullptr);
        if (hThread) { WaitForSingleObject(hThread, INFINITE); CloseHandle(hThread); }
    }

    void MainWindow::CreateTab()
    {
        if (!m_app || !m_hwnd) return;
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
        tv.SelectedItem(item);

        auto app = m_app;
        auto hwnd = m_hwnd;
        auto weakThis = get_weak();
        // Hold strong refs so the lambda can keep them alive until Loaded fires.
        auto panelStrong = panel;
        auto itemStrong = item;

        panel.Loaded([weakThis, panelStrong, itemStrong, app, hwnd](auto&&, auto&&) {
            auto self = weakThis.get();
            if (!self) return;

            // Loaded can fire again on layout updates — bail if already constructed.
            if (self->m_tabs.FindByItem(itemStrong)) return;

            // Wrap Tab::Create in SEH guard so a hardware exception in the
            // NVIDIA driver during ghostty_surface_new (e.g. dx_create_texture
            // crash) doesn't kill the whole app and take every other tab
            // with it. The C++ work happens inside the callback below.
            struct CreateCtx {
                muxc::SwapChainPanel const* panel;
                muxc::TabViewItem const* item;
                ghostty_app_t app;
                HWND hwnd;
                std::unique_ptr<Tab> result;
            };
            CreateCtx ctx{ &panelStrong, &itemStrong, app, hwnd, nullptr };
            int ok = RunSEHGuarded([](void* arg) noexcept {
                auto* c = static_cast<CreateCtx*>(arg);
                c->result = Tab::Create(*c->panel, *c->item, c->app, c->hwnd);
            }, &ctx);

            std::unique_ptr<Tab> tab = std::move(ctx.result);
            if (!ok) {
                // SEH caught a hardware exception inside Tab::Create — almost
                // always the NVIDIA driver memcpy crash. Process state is
                // unreliable from here (heap locks may be stuck, driver
                // kernel state corrupted, etc.) so don't try to continue.
                //
                // Hide the main window first — its XAML/D3D state may be
                // partially broken and showing it next to the message box
                // looks alarming. The dialog is parented to nullptr so it
                // stays visible after we hide the window.
                if (self->m_hwnd) ShowWindow(self->m_hwnd, SW_HIDE);
                MessageBoxW(nullptr,
                    L"A graphics driver error occurred while creating the new tab.\n"
                    L"GhosttyWin32 will exit safely.\n\n"
                    L"Restarting the app usually recovers — the next launch\n"
                    L"will automatically wait 2 seconds for the driver.",
                    L"GhosttyWin32",
                    MB_OK | MB_ICONERROR | MB_TASKMODAL);
                if (self->m_hwnd) {
                    PostMessageW(self->m_hwnd, WM_CLOSE, 0, 0);
                }
                return;
            }
            if (!tab) {
                // Tab::Create returned null cleanly (handle / attach / surface
                // creation failed but no hardware exception). Heap state is
                // intact, so just drop the orphan tab item and continue.
                auto items = self->TabView().TabItems();
                uint32_t idx = 0;
                if (items.IndexOf(itemStrong, idx)) items.RemoveAt(idx);
                return;
            }

            tab->Focus();
            ShowWindow(hwnd, SW_SHOW);
            if (self->m_editContext) {
                self->m_ime.reset();
                self->m_editContext.NotifyFocusEnter();
            }

            self->m_tabs.Add(std::move(tab));
        });
    }
}
