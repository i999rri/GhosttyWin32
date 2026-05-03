#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include <filesystem>
#include <fstream>
#include <windows.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace {
    std::filesystem::path crashFlagPathApp() {
        wchar_t buf[MAX_PATH];
        DWORD len = GetTempPathW(MAX_PATH, buf);
        if (len == 0) return L"GhosttyWin32_running.flag";
        return std::filesystem::path(buf) / L"GhosttyWin32_running.flag";
    }
}

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace winrt::GhosttyWin32::implementation
{
    /// <summary>
    /// Initializes the singleton application object.  This is the first line of authored code
    /// executed, and as such is the logical equivalent of main() or WinMain().
    /// </summary>
    App::App()
    {
        // Xaml objects should not call InitializeComponent during construction.
        // See https://github.com/microsoft/cppwinrt/tree/master/nuget#initializecomponent

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                OutputDebugStringW((L"[Ghostty] Unhandled: " + errorMessage + L"\n").c_str());
            }
            e.Handled(true);
        });
#endif
    }

    /// <summary>
    /// Invoked when the application is launched.
    /// </summary>
    /// <param name="e">Details about the launch request and process.</param>
    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        // Crash recovery BEFORE creating the window: if the previous run
        // didn't reach clean shutdown, give the GPU driver time to recover
        // its kernel-side state. Doing this here (no XAML window yet) avoids
        // a visible white flash that would happen if we slept inside the
        // Activated handler — by then the window is already mapped.
        {
            std::error_code ec;
            auto flag = crashFlagPathApp();
            if (std::filesystem::exists(flag, ec)) {
                OutputDebugStringA("GhosttyWin32: previous run crashed; pausing 2s for driver recovery\n");
                Sleep(2000);
            }
            std::ofstream(flag).close();
        }

        window = make<MainWindow>();
        window.Activate();
    }
}
