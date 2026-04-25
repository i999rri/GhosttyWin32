#include "pch.h"
#include "MainWindow.xaml.h"
#include "Clipboard.h"
#include "KeyModifiers.h"
#include "Encoding.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include <microsoft.ui.xaml.window.h>
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dcomp.lib")

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

                    auto* sess = ActiveSession();
                    if (!sess || !sess->surface) return;

                    if (m_ime.composing()) {
                        if (m_ime.text().empty()) {
                            ghostty_surface_preedit(sess->surface, nullptr, 0);
                        } else {
                            auto utf8 = Encoding::toUtf8(m_ime.text());
                            if (!utf8.empty())
                                ghostty_surface_preedit(sess->surface, utf8.c_str(), utf8.size());
                        }
                    }
                    if (m_app) ghostty_app_tick(m_app);
                    ghostty_surface_refresh(sess->surface);
                });

                m_editContext.CompositionStarted([this](txtCore::CoreTextEditContext const&, txtCore::CoreTextCompositionStartedEventArgs const&) {
                    m_ime.compositionStarted();
                });

                m_editContext.CompositionCompleted([this](txtCore::CoreTextEditContext const&, txtCore::CoreTextCompositionCompletedEventArgs const&) {
                    auto* sess = ActiveSession();
                    if (sess && sess->surface) {
                        ghostty_surface_preedit(sess->surface, nullptr, 0);
                        auto utf8 = Encoding::toUtf8(m_ime.text());
                        if (!utf8.empty()) {
                            ghostty_surface_text(sess->surface, utf8.c_str(), utf8.size());
                        }
                        if (m_app) ghostty_app_tick(m_app);
                        ghostty_surface_refresh(sess->surface);
                    }
                    m_ime.compositionCompleted();
                });

                m_editContext.LayoutRequested([this](txtCore::CoreTextEditContext const&, txtCore::CoreTextLayoutRequestedEventArgs const& args) {
                    auto* sess = ActiveSession();
                    if (!sess || !sess->surface || !m_hwnd) return;
                    double x = 0, y = 0, w = 0, h = 0;
                    ghostty_surface_ime_point(sess->surface, &x, &y, &w, &h);
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
                        auto* sess = ActiveSession();
                        if (sess && sess->surface)
                            ghostty_surface_preedit(sess->surface, nullptr, 0);
                    }
                });
            }

            auto tv = TabView();
            SetTitleBar(DragRegion());




            // Window-level input handling (same approach as Windows Terminal)
            auto root = Content().as<winrt::Microsoft::UI::Xaml::UIElement>();

            root.KeyDown([this](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                auto* sess = ActiveSession();
                if (!sess || !sess->surface) return;

                int vk = static_cast<int>(args.Key());
                UINT scanCode = args.KeyStatus().ScanCode;
                bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
                bool shift = GetKeyState(VK_SHIFT) & 0x8000;

                // IME is processing this key
                if (vk == VK_PROCESSKEY || m_ime.composing()) return;

                // Ctrl+C: copy if selection exists, otherwise send SIGINT
                if (ctrl && !shift && vk == 'C') {
                    if (ghostty_surface_has_selection(sess->surface)) {
                        ghostty_text_s text = {};
                        if (ghostty_surface_read_selection(sess->surface, &text) && text.text && text.text_len > 0) {
                            Clipboard::write(m_hwnd, Encoding::toUtf16(text.text, static_cast<int>(text.text_len)));
                            ghostty_surface_free_text(sess->surface, &text);
                        }
                        ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                        ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                        args.Handled(true);
                        return;
                    }
                }

                // Ctrl+V: paste from clipboard
                if (ctrl && !shift && vk == 'V') {
                    auto utf8 = Encoding::toUtf8(Clipboard::read(m_hwnd));
                    if (!utf8.empty()) {
                        ghostty_surface_text(sess->surface, utf8.c_str(), utf8.size());
                    }
                    if (m_app) ghostty_app_tick(m_app);
                    ghostty_surface_refresh(sess->surface);
                    args.Handled(true);
                    return;
                }

                // Send key event to ghostty
                ghostty_input_key_s keyEvent = {};
                keyEvent.action = GHOSTTY_ACTION_PRESS;
                keyEvent.keycode = scanCode;
                if (args.KeyStatus().IsExtendedKey) keyEvent.keycode |= 0xE000;
                keyEvent.mods = currentMods();
                ghostty_surface_key(sess->surface, keyEvent);

                // Translate to text using ToUnicode (replaces CharacterReceived)
                BYTE kbState[256] = {};
                GetKeyboardState(kbState);
                wchar_t chars[4] = {};
                int charCount = ToUnicode(vk, scanCode, kbState, chars, 4, 0);
                if (charCount > 0 && chars[0] >= 0x20) {
                    char utf8[16] = {};
                    int len = WideCharToMultiByte(CP_UTF8, 0, chars, charCount, utf8, sizeof(utf8), nullptr, nullptr);
                    if (len > 0) {
                        ghostty_surface_text(sess->surface, utf8, len);
                    }
                }

                if (m_app) ghostty_app_tick(m_app);
                ghostty_surface_refresh(sess->surface);
                args.Handled(true);
            });

            // Mouse input on root
            root.PointerMoved([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                auto* sess = ActiveSession();
                if (!sess || !sess->surface) return;
                winrt::Microsoft::UI::Input::PointerPoint point = args.GetCurrentPoint(sess->panel);
                winrt::Windows::Foundation::Point pos = point.Position();
                ghostty_surface_mouse_pos(sess->surface, pos.X, pos.Y, currentMods());
            });

            root.PointerPressed([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                auto* sess = ActiveSession();
                if (!sess || !sess->surface) return;
                winrt::Microsoft::UI::Input::PointerPoint point = args.GetCurrentPoint(sess->panel);
                winrt::Microsoft::UI::Input::PointerPointProperties props = point.Properties();
                ghostty_input_mouse_button_e btn;
                if (props.IsLeftButtonPressed()) btn = GHOSTTY_MOUSE_LEFT;
                else if (props.IsRightButtonPressed()) {
                    // Right-click: copy selection if exists
                    if (ghostty_surface_has_selection(sess->surface)) {
                        ghostty_text_s text = {};
                        if (ghostty_surface_read_selection(sess->surface, &text) && text.text && text.text_len > 0) {
                            Clipboard::write(m_hwnd, Encoding::toUtf16(text.text, static_cast<int>(text.text_len)));
                            ghostty_surface_free_text(sess->surface, &text);
                        }
                        ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                        ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, (ghostty_input_mods_e)0);
                        return;
                    }
                    btn = GHOSTTY_MOUSE_RIGHT;
                }
                else return; // Ignore middle-click and others
                ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_PRESS, btn, currentMods());
            });

            root.PointerReleased([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {
                auto* sess = ActiveSession();
                if (!sess || !sess->surface) return;
                ghostty_surface_mouse_button(sess->surface, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT, currentMods());
            });

            root.PointerWheelChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                auto* sess = ActiveSession();
                if (!sess || !sess->surface) return;
                winrt::Microsoft::UI::Input::PointerPoint point = args.GetCurrentPoint(sess->panel);
                winrt::Microsoft::UI::Input::PointerPointProperties props = point.Properties();
                int delta = props.MouseWheelDelta();
                double scrollY = (double)delta / 120.0;
                ghostty_input_scroll_mods_t smods = {};
                ghostty_surface_mouse_scroll(sess->surface, 0, scrollY, smods);
                args.Handled(true);
            });

            root.KeyUp([this](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                auto* sess = ActiveSession();
                if (!sess || !sess->surface) return;
                ghostty_input_key_s keyEvent = {};
                keyEvent.action = GHOSTTY_ACTION_RELEASE;
                keyEvent.keycode = args.KeyStatus().ScanCode;
                if (args.KeyStatus().IsExtendedKey) keyEvent.keycode |= 0xE000;
                keyEvent.mods = currentMods();
                ghostty_surface_key(sess->surface, keyEvent);
            });

            // DPI change handling (deferred until XamlRoot is available)
            Content().as<winrt::Microsoft::UI::Xaml::FrameworkElement>().Loaded([this](auto&&, auto&&) {
                Content().XamlRoot().Changed([this](auto&&, winrt::Microsoft::UI::Xaml::XamlRootChangedEventArgs const&) {
                    if (!m_hwnd) return;
                    UINT dpi = GetDpiForWindow(m_hwnd);
                    double scale = (double)dpi / 96.0;
                    for (auto& s : m_sessions) {
                        if (s->surface) {
                            ghostty_surface_set_content_scale(s->surface, scale, scale);
                        }
                    }
                });
            });

            tv.AddTabButtonClick([this](muxc::TabView const&, auto&&) {
                CreateTab();
            });

            tv.TabCloseRequested([this](muxc::TabView const& sender, muxc::TabViewTabCloseRequestedEventArgs const& args) {
                auto tab = args.Tab();
                auto content = tab.Content();
                uint32_t tabIdx = 0;
                sender.TabItems().IndexOf(tab, tabIdx);

                for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
                    if (content && (*it)->panel == content.as<muxc::SwapChainPanel>()) {
                        sender.TabItems().RemoveAt(tabIdx);

                        // Detach panel from surface handle before tearing down.
                        if ((*it)->panel) {
                            if (auto native2 = (*it)->panel.try_as<ISwapChainPanelNative2>()) {
                                native2->SetSwapChainHandle(nullptr);
                            }
                        }
                        (*it)->panel = nullptr;
                        DwmFlush();

                        // ghostty_surface_free joins renderer + IO threads,
                        // then destroys the device + swap chain it owns.
                        if ((*it)->surface) ghostty_surface_free((*it)->surface);

                        if ((*it)->surfaceHandle) CloseHandle((*it)->surfaceHandle);
                        m_sessions.erase(it);
                        break;
                    }
                }
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
        for (auto& s : m_sessions) {
            if (s->panel) {
                if (auto native2 = s->panel.try_as<ISwapChainPanelNative2>()) {
                    native2->SetSwapChainHandle(nullptr);
                }
            }
            if (s->surface) ghostty_surface_free(s->surface);
            if (s->surfaceHandle) CloseHandle(s->surfaceHandle);
        }
        if (m_app) ghostty_app_free(m_app);
        if (m_config) ghostty_config_free(m_config);
        if (m_dxgiAdapter) { m_dxgiAdapter->Release(); m_dxgiAdapter = nullptr; }
        if (m_dxgiFactory) { m_dxgiFactory->Release(); m_dxgiFactory = nullptr; }
    }

    TabSession* MainWindow::ActiveSession()
    {
        auto tv = TabView();
        auto sel = tv.SelectedItem();
        if (!sel) return nullptr;
        auto tab = sel.as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
        if (!tab) return nullptr;
        auto content = tab.Content();
        if (!content) return nullptr;
        // Match by panel reference — works even after tab reorder
        for (auto& s : m_sessions) {
            if (s->panel == content.as<winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel>()) {
                return s.get();
            }
        }
        return nullptr;
    }

    void MainWindow::InitGhostty()
    {
        // Cache DXGI factory and adapter for per-tab device creation
        CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void**)&m_dxgiFactory);
        if (m_dxgiFactory) {
            m_dxgiFactory->EnumAdapters(0, &m_dxgiAdapter);
        }

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
                                    auto tv = mw->TabView();
                                    for (uint32_t i = 0; i < tv.TabItems().Size() && i < mw->m_sessions.size(); i++) {
                                        if (mw->m_sessions[i]->surface == surface) {
                                            auto tab = tv.TabItems().GetAt(i).as<muxc::TabViewItem>();
                                            tab.Header(box_value(winrt::hstring(*wstr)));
                                            break;
                                        }
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
                    auto* sess = g_mainWindow->ActiveSession();
                    if (!sess || !sess->surface) return false;
                    auto utf8 = Encoding::toUtf8(Clipboard::read(g_mainWindow->m_hwnd));
                    if (utf8.empty()) return false;
                    ghostty_surface_complete_clipboard_request(sess->surface, utf8.c_str(), state, false);
                    return true;
                };
                rtConfig.confirm_read_clipboard_cb = [](void*, const char* content, void* state, ghostty_clipboard_request_e) {
                    // Auto-confirm clipboard reads
                    if (g_mainWindow) {
                        auto* sess = g_mainWindow->ActiveSession();
                        if (sess && sess->surface) {
                            ghostty_surface_complete_clipboard_request(sess->surface, content, state, true);
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

        auto session = std::make_unique<TabSession>();
        session->panel = panel;
        size_t sessionIdx = m_sessions.size();
        m_sessions.push_back(std::move(session));

        auto tab = muxc::TabViewItem();
        static constexpr wchar_t kDefaultTabTitle[] = L" ";
        tab.Header(box_value(kDefaultTabTitle));
        tab.IsClosable(true);
        tab.Content(panel);
        tv.TabItems().Append(tab);
        tv.SelectedItem(tab);

        auto app = m_app;
        auto hwnd = m_hwnd;
        auto weakThis = get_weak();

        panel.Loaded([sessionIdx, app, hwnd, weakThis](auto&& sender, auto&&) {
            auto self = weakThis.get();
            if (!self) return;
            if (sessionIdx >= self->m_sessions.size()) return;
            auto* sess = self->m_sessions[sessionIdx].get();
            if (sess->surface) return;

            auto p = sender.as<muxc::SwapChainPanel>();

            // Synchronous tab creation, all on UI thread:
            //   1. Create DComp surface handle
            //   2. Bind to panel via ISwapChainPanelNative2::SetSwapChainHandle
            //   3. Pass handle to ghostty — ghostty's renderer thread will
            //      create device + swap chain itself (single-thread per device,
            //      matches Windows Terminal AtlasEngine pattern that NVIDIA's
            //      driver hot path is tested against).
            HANDLE surfaceHandle = nullptr;
            constexpr DWORD COMPOSITIONSURFACE_ALL_ACCESS = 0x0003L;
            if (FAILED(DCompositionCreateSurfaceHandle(COMPOSITIONSURFACE_ALL_ACCESS, nullptr, &surfaceHandle))) {
                OutputDebugStringA("D3D11: DCompositionCreateSurfaceHandle FAILED\n");
                return;
            }

            HRESULT hrAttach = E_FAIL;
            if (auto native2 = p.try_as<ISwapChainPanelNative2>()) {
                hrAttach = native2->SetSwapChainHandle(surfaceHandle);
            }
            {
                char buf[128];
                sprintf_s(buf, "D3D11: SetSwapChainHandle hr=0x%08X handle=%p\n",
                          (unsigned)hrAttach, surfaceHandle);
                OutputDebugStringA(buf);
            }
            if (FAILED(hrAttach)) {
                CloseHandle(surfaceHandle);
                return;
            }

            sess->surfaceHandle = surfaceHandle;

            ghostty_surface_config_s cfg = ghostty_surface_config_new();
            cfg.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
            cfg.platform.windows.hwnd = hwnd;
            cfg.platform.windows.composition_surface_handle = surfaceHandle;
            UINT dpi = GetDpiForWindow(hwnd);
            cfg.scale_factor = (double)dpi / 96.0;
            sess->surface = ghostty_surface_new(app, &cfg);
            if (!sess->surface) {
                OutputDebugStringA("D3D11: ghostty_surface_new FAILED\n");
                return;
            }

            p.Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
            ShowWindow(hwnd, SW_SHOW);

            if (g_mainWindow && g_mainWindow->m_editContext) {
                g_mainWindow->m_ime.reset();
                g_mainWindow->m_editContext.NotifyFocusEnter();
            }

            auto surface = sess->surface;
            p.SizeChanged([surface](auto&&, winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args) {
                auto newSize = args.NewSize();
                uint32_t w = static_cast<uint32_t>(newSize.Width);
                uint32_t h = static_cast<uint32_t>(newSize.Height);
                if (w > 0 && h > 0) {
                    ghostty_surface_set_size(surface, w, h);
                }
            });

            uint32_t pw = static_cast<uint32_t>(p.ActualWidth());
            uint32_t ph = static_cast<uint32_t>(p.ActualHeight());
            if (pw > 0 && ph > 0) {
                ghostty_surface_set_size(surface, pw, ph);
            }
        });
    }
}
