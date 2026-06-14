#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Ghostty/RuntimeConfigFactory.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Microsoft.Windows.AppNotifications.h>

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
    App* App::g_app = nullptr;

    /// <summary>
    /// Initializes the singleton application object.  This is the first line of authored code
    /// executed, and as such is the logical equivalent of main() or WinMain().
    /// </summary>
    App::App()
    {
        g_app = this;
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

    void App::OnInstanceActivated(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments const& args)
    {
        // Activated fires on a worker — bounce to the UI thread before
        // touching the window. If the redirect happened before OnLaunched
        // wired up `window`, just drop it; OnLaunched will run normally
        // and present the terminal as a first-launch effect.
        if (!window) return;
        window.DispatcherQueue().TryEnqueue([weak = get_weak(), args]() {
            if (auto self = weak.get()) {
                self->HandleActivation(args);
            }
        });
    }

    void App::HandleActivation(
        winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments const&)
    {
        // The activation args delivered through AppInstance::Activated
        // are a cross-process proxy back to the redirecting instance —
        // which has already exited by the time we get here, so any
        // Kind() / Data() call would throw RPC_S_SERVER_UNAVAILABLE.
        // We deliberately ignore the args and just bring the window
        // forward; the surface-targeted route (clicks on toasts we
        // raised ourselves) is handled by OnNotificationInvoked, where
        // the args are in-process and safe to read.
        if (auto mwProj = window.try_as<winrt::GhosttyWin32::MainWindow>()) {
            if (auto* mw = winrt::get_self<implementation::MainWindow>(mwProj)) {
                mw->PresentNotification(implementation::PaneId{ 0 });
            }
        }
    }

    void App::RouteNotificationClick(uint64_t paneIdValue)
    {
        // Called from the wWinMain-side NotificationInvoked subscriber
        // on a worker thread. Bounce to the UI thread before touching
        // the window. If the click arrives before OnLaunched has
        // wired `window`, drop it — the user just gets the regular
        // foreground from AppInstance::Activated.
        if (!window) return;
        window.DispatcherQueue().TryEnqueue(
            [weak = get_weak(), paneIdValue]()
            {
                auto self = weak.get();
                if (!self) return;
                if (auto mwProj = self->window.try_as<winrt::GhosttyWin32::MainWindow>()) {
                    if (auto* mw = winrt::get_self<implementation::MainWindow>(mwProj)) {
                        mw->PresentNotification(implementation::PaneId{ paneIdValue });
                    }
                }
            });
    }

    uint64_t App::ParseSurfaceIdFromArguments(std::wstring const& arguments)
    {
        // Argument format is `key=value;key=value`. Today only
        // `surfaceId=<u64>` is emitted; the generic loop lets future
        // fields piggy-back without a rewrite.
        uint64_t paneIdValue = 0;
        size_t pos = 0;
        while (pos < arguments.size()) {
            auto sep = arguments.find(L';', pos);
            std::wstring pair = arguments.substr(pos, sep == std::wstring::npos
                                                 ? std::wstring::npos
                                                 : sep - pos);
            auto eq = pair.find(L'=');
            if (eq != std::wstring::npos) {
                auto key = pair.substr(0, eq);
                auto val = pair.substr(eq + 1);
                if (key == L"surfaceId" && !val.empty()) {
                    try {
                        paneIdValue = std::stoull(val);
                    } catch (...) {
                        paneIdValue = 0;
                    }
                }
            }
            if (sep == std::wstring::npos) break;
            pos = sep + 1;
        }
        return paneIdValue;
    }

    /// <summary>
    /// Invoked when the application is launched.
    /// </summary>
    /// <param name="e">Details about the launch request and process.</param>
    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        // Single-instance redirect already ran in wWinMain (see main.cpp);
        // if we got here, we're the primary instance. Just wire the
        // Activated event so subsequent launches (which wWinMain bounced
        // back to us via RedirectActivationToAsync) land in
        // HandleActivation instead of being silently dropped. We
        // re-acquire `primary` via FindOrRegisterForKey — same key, same
        // holder, idempotent.
        {
            namespace appLifecycle = winrt::Microsoft::Windows::AppLifecycle;
            auto primary = appLifecycle::AppInstance::FindOrRegisterForKey(L"GhosttyWin32-Main");
            auto weak = get_weak();
            m_activatedToken = primary.Activated(
                [weak](winrt::Windows::Foundation::IInspectable const& sender,
                       appLifecycle::AppActivationArguments const& a)
                {
                    if (auto self = weak.get()) {
                        self->OnInstanceActivated(sender, a);
                    }
                });
        }

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

        // AppNotificationManager::Default().Register() and the
        // NotificationInvoked subscription both already happened in
        // wWinMain (see main.cpp). Windows App SDK 1.8 throws and
        // fail-fasts if we try to re-subscribe NotificationInvoked
        // here after Register(), so the routing path goes via the
        // wWinMain-side subscriber calling App::RouteNotificationClick.

        // Bring up ghostty BEFORE constructing the window. The
        // runtime config's callbacks reach host state via the
        // `g_mainWindow` static, so they can be wired without a
        // MainWindow existing yet — the first callback won't fire
        // until a surface is created, by which point Activate has
        // run and the static is set. Doing the order this way means
        // MainWindow's constructor can already see a live
        // `App::Ghostty()` and adopt it as an invariant.
        m_ghostty = core::ghostty::App::Create(
            implementation::RuntimeConfigFactory::Build());
        if (!m_ghostty) {
            OutputDebugStringW(L"[App] ghostty init failed; not creating window\n");
            return;
        }

        window = make<MainWindow>();
        window.Activate();
    }
}
