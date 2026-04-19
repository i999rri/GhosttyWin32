#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include <microsoft.ui.xaml.window.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace muxc = Microsoft::UI::Xaml::Controls;

namespace winrt::GhosttyWin32::implementation
{
    MainWindow::MainWindow()
    {
        ExtendsContentIntoTitleBar(true);

        Activated([this](auto&&, auto&&) {
            static bool initialized = false;
            if (initialized) return;
            initialized = true;

            auto tv = TabView();
            SetTitleBar(tv);

            tv.AddTabButtonClick([this](muxc::TabView const&, auto&&) {
                CreateTab();
            });

            tv.TabCloseRequested([this](muxc::TabView const& sender, muxc::TabViewTabCloseRequestedEventArgs const& args) {
                uint32_t idx = 0;
                if (sender.TabItems().IndexOf(args.Tab(), idx)) {
                    // TODO: destroy ghostty surface for this tab
                    sender.TabItems().RemoveAt(idx);
                }
                if (sender.TabItems().Size() == 0) {
                    this->Close();
                }
            });

            InitGhostty();

            // Replace the template tab with a real ghostty tab
            tv.TabItems().Clear();
            CreateTab();
        });
    }

    MainWindow::~MainWindow()
    {
        if (m_app) ghostty_app_free(m_app);
        if (m_config) ghostty_config_free(m_config);
    }

    void MainWindow::InitGhostty()
    {
        // ghostty requires 4MB stack for Zig runtime
        struct Args { MainWindow* self; };
        Args args{ this };

        HANDLE hThread = CreateThread(nullptr, 4 * 1024 * 1024,
            [](LPVOID param) -> DWORD {
                auto* a = static_cast<Args*>(param);

                ghostty_init(0, nullptr);

                ghostty_runtime_config_s rtConfig{};
                rtConfig.wakeup_cb = [](void*) {};
                rtConfig.action_cb = [](ghostty_app_t, ghostty_target_s, ghostty_action_s) -> bool { return false; };
                rtConfig.read_clipboard_cb = [](void*, ghostty_clipboard_e, void*) -> bool { return false; };
                rtConfig.confirm_read_clipboard_cb = [](void*, const char*, void*, ghostty_clipboard_request_e) {};
                rtConfig.write_clipboard_cb = [](void*, ghostty_clipboard_e, const ghostty_clipboard_content_s*, size_t, bool) {};
                rtConfig.close_surface_cb = [](void*, bool) {};

                a->self->m_config = ghostty_config_new();
                ghostty_config_finalize(a->self->m_config);

                a->self->m_app = ghostty_app_new(&rtConfig, a->self->m_config);
                return 0;
            }, &args, 0, nullptr);

        if (hThread) {
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
        }
    }

    void MainWindow::CreateTab()
    {
        if (!m_app) return;

        auto tv = TabView();

        // Get the Window HWND for ghostty surface creation
        auto windowNative = this->try_as<::IWindowNative>();
        HWND hwnd = nullptr;
        if (windowNative) windowNative->get_WindowHandle(&hwnd);
        if (!hwnd) return;

        // Create SwapChainPanel for this tab
        auto panel = muxc::SwapChainPanel();

        // Create ghostty surface on 4MB stack thread (Zig requirement)
        struct SurfArgs {
            ghostty_app_t app;
            HWND hwnd;
            ghostty_surface_t surface;
        };
        SurfArgs sargs{ m_app, hwnd, nullptr };

        HANDLE hThread = CreateThread(nullptr, 4 * 1024 * 1024,
            [](LPVOID param) -> DWORD {
                auto* a = static_cast<SurfArgs*>(param);
                ghostty_surface_config_s surfConfig = ghostty_surface_config_new();
                surfConfig.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
                surfConfig.platform.windows.hwnd = a->hwnd;
                UINT dpi = GetDpiForWindow(a->hwnd);
                surfConfig.scale_factor = (double)dpi / 96.0;
                a->surface = ghostty_surface_new(a->app, &surfConfig);
                return 0;
            }, &sargs, 0, nullptr);

        if (hThread) {
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
        }
        if (!sargs.surface) return;
        auto surface = sargs.surface;

        // Get swap chain from ghostty and set it on the SwapChainPanel
        void* swapChainPtr = ghostty_surface_swap_chain(surface);
        if (swapChainPtr) {
            auto panelNative = panel.as<ISwapChainPanelNative>();
            panelNative->SetSwapChain(reinterpret_cast<IDXGISwapChain*>(swapChainPtr));
        }

        // Create tab
        auto tab = muxc::TabViewItem();
        tab.Header(box_value(L"Terminal"));
        tab.IsClosable(true);
        tab.Content(panel);

        tv.TabItems().Append(tab);
        tv.SelectedItem(tab);
    }
}
