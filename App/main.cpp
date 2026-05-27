#include "pch.h"

#include <windows.h>
#include <combaseapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Microsoft.Windows.AppNotifications.h>

#include "App.xaml.h"

// Custom wWinMain so the AppInstance single-instance redirect runs
// BEFORE Microsoft::UI::Xaml::Application::Start. Doing the redirect
// after Application::Start (the XAML compiler's default entry point
// is structured that way) trips a WIL fail-fast inside
// Microsoft.WindowsAppRuntime.dll at wil/resource.h:2772 — the cross-
// process shared-memory event AppInstance uses races on
// WaitForSingleObjectEx once the XAML / COM context has been brought
// up. See microsoft/WindowsAppSDK issues #1709 / #1766.
int APIENTRY wWinMain(_In_ HINSTANCE,
                      _In_opt_ HINSTANCE,
                      _In_ LPWSTR,
                      _In_ int)
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Register the AppNotificationManager BEFORE AppInstance::GetActivatedEventArgs
    // is called. Second-launch activations arrive wrapped in
    // IProtocolActivatedEventArgs, and AppInstance's GetActivatedEventArgs
    // implementation decodes them by routing through
    // AppNotificationManager::AppNotificationDeserialize. Inside that path
    // a wil::event_t::wait() blocks for the AppNotifications COM server to
    // be ready — if Register() hasn't been called yet the wait never
    // completes and WIL fail-fasts at resource.h:2772 (E_UNEXPECTED).
    //
    // Register() in turn requires NotificationInvoked to already have at
    // least one subscriber, otherwise it throws "Must register event
    // handlers before calling Register()" (ERROR_NOT_FOUND). The App
    // instance doesn't exist yet, so we install a no-op handler now to
    // satisfy that invariant; App::OnLaunched adds the real one on top
    // afterwards (both handlers fire on a real click — the dummy is a
    // safe extra no-op).
    {
        namespace appNotif = winrt::Microsoft::Windows::AppNotifications;
        // Subscribe BEFORE Register so the SDK's "Must register event
        // handlers before calling Register()" invariant is satisfied.
        // Windows App SDK 1.8 also fail-fasts if anything tries to
        // re-subscribe after Register(), so this is also the ONLY
        // NotificationInvoked subscription in the whole app — the
        // routing into the live App instance happens via the global
        // App::g_app pointer set in App::App() (no member-function
        // capture needed here since the App doesn't exist yet).
        appNotif::AppNotificationManager::Default().NotificationInvoked(
            [](appNotif::AppNotificationManager const&,
               appNotif::AppNotificationActivatedEventArgs const& args)
            {
                std::wstring arguments = std::wstring(args.Argument());
                uint64_t paneIdValue =
                    winrt::GhosttyWin32::implementation::App::ParseSurfaceIdFromArguments(arguments);
                if (auto* app = winrt::GhosttyWin32::implementation::App::g_app) {
                    app->RouteNotificationClick(paneIdValue);
                }
            });
        try {
            appNotif::AppNotificationManager::Default().Register();
        } catch (winrt::hresult_error const&) {
            // Best-effort; if registration fails the toast subsystem just
            // doesn't show toasts. We let AppInstance try anyway.
        }
    }

    {
        namespace AL = winrt::Microsoft::Windows::AppLifecycle;
        auto args = AL::AppInstance::GetCurrent().GetActivatedEventArgs();
        auto primary = AL::AppInstance::FindOrRegisterForKey(L"GhosttyWin32-Main");
        if (!primary.IsCurrent()) {
            // RedirectActivationToAsync's IAsyncAction can't be
            // awaited from this thread directly — the call only
            // completes once the primary instance dispatches our
            // incoming RPC, and that requires *us* to keep pumping
            // COM messages. Run .get() on a worker thread and
            // CoWaitForMultipleObjects(CWMO_DISPATCH_CALLS) here so
            // the wait both pumps and unblocks on the worker's
            // completion event.
            HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            struct Ctx {
                AL::AppInstance primary;
                AL::AppActivationArguments args;
                HANDLE done;
            };
            auto* ctx = new Ctx{ primary, args, done };
            HANDLE thread = ::CreateThread(nullptr, 0,
                [](LPVOID p) -> DWORD {
                    auto* c = static_cast<Ctx*>(p);
                    c->primary.RedirectActivationToAsync(c->args).get();
                    ::SetEvent(c->done);
                    delete c;
                    return 0;
                }, ctx, 0, nullptr);
            DWORD idx = 0;
            HANDLE waits[1] = { done };
            ::CoWaitForMultipleObjects(CWMO_DISPATCH_CALLS, INFINITE, 1, waits, &idx);
            if (thread) ::CloseHandle(thread);
            ::CloseHandle(done);
            ::ExitProcess(0);
        }
        // We're primary; the App class subscribes to primary.Activated
        // from OnLaunched (it can't subscribe here — the App instance
        // doesn't exist until Application::Start runs the lambda below).
    }

    winrt::Microsoft::UI::Xaml::Application::Start(
        [](auto&&) {
            winrt::make<winrt::GhosttyWin32::implementation::App>();
        });
    return 0;
}
