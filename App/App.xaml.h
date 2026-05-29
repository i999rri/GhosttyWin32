#pragma once

#include "App.xaml.g.h"
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Microsoft.Windows.AppNotifications.h>
#include <string>

namespace winrt::GhosttyWin32::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

        // Single live App instance used by the wWinMain-side
        // AppNotificationManager subscriber to call back into us.
        // Application::Start runs `make<App>` once for the process, so
        // we cache that one pointer in a global on ctor / clear on
        // exit. Raw pointer (not weak_ref) because the
        // NotificationInvoked subscriber runs from a worker thread
        // where touching a com_ptr isn't always safe.
        static App* g_app;

        // Called from the wWinMain-side AppNotificationManager
        // subscriber. Public because that subscriber is a free
        // function in main.cpp — it can't reach a private member.
        // Windows App SDK 1.8 forbids a second NotificationInvoked
        // subscription after Register() (it fatal-fasts), so the
        // single subscriber has to be the one in wWinMain.
        void RouteNotificationClick(uint64_t paneIdValue);

        // Shared parse of a notification's argument string into a
        // PaneId value. Public for the same reason as above —
        // wWinMain calls this before routing into RouteNotificationClick.
        // Argument format is `key=value;key=value`; currently the only
        // key is `surfaceId`. Returns 0 (no retargeting) on any
        // decode failure.
        static uint64_t ParseSurfaceIdFromArguments(std::wstring const& arguments);

    private:
        // Subsequent-activation handler. The first activation runs through
        // OnLaunched; later activations (a second click of a notification,
        // a relaunch from the Start menu, etc.) get redirected to this
        // primary instance by `AppInstance::RedirectActivationToAsync` in
        // the ctor and arrive here.
        void OnInstanceActivated(
            winrt::Windows::Foundation::IInspectable const&,
            winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments const& args);

        // Foreground the window without touching the activation args.
        // Used for the AppInstance::Activated redirect path where the
        // args are a cross-process proxy whose forwarder has already
        // exited (so Kind() / Data() throw RPC_S_SERVER_UNAVAILABLE).
        // Tab-switching by surface id happens through the in-process
        // NotificationInvoked path below, where args are valid.
        void HandleActivation(
            winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments const& args);

        winrt::Microsoft::UI::Xaml::Window window{ nullptr };
        // Token for the primary AppInstance's Activated event; the
        // subscription lives for the lifetime of the App.
        winrt::event_token m_activatedToken{};
    };
}
