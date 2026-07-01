#pragma once

#include "App.xaml.g.h"
#include "Ghostty/App.h"
#include "MainWindows.h"
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Microsoft.Windows.AppNotifications.h>
#include <memory>
#include <string>

namespace winrt::GhosttyWin32::implementation
{
    class MainWindowRuntime;

    struct App : AppT<App>
    {
        App();
        // Defined in App.xaml.cpp where MainWindowRuntime's full
        // definition is visible — std::unique_ptr<MainWindowRuntime>'s
        // implicit destructor needs the complete type, and the header
        // only forward-declares it to avoid pulling the heavy
        // Ghostty/MainWindowRuntime.h dependency into every translation
        // unit that includes App.xaml.h.
        ~App();

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

        // Borrowed accessor for the process-wide ghostty::App.
        // OnLaunched creates it before make<MainWindow>() and aborts
        // if creation fails, so any code path that can see a live
        // MainWindow sees a non-null result. App owns the wrapper and
        // tears it down in its destructor, AFTER the window member
        // (declared below this one) has released every TerminalControl
        // and the surfaces they held.
        core::ghostty::App* Ghostty() const noexcept { return m_ghostty.get(); }

        // Aggregate for the set of live MainWindows. Sibling to
        // `Tabs` in shape: a borrow-only collection that MainWindow
        // registers itself into during its Activated handler and
        // unregisters from in its destructor. Runtime callbacks and
        // target-based routing consult this to reach the right
        // window.
        MainWindows&       Windows()       noexcept { return m_windows; }
        MainWindows const& Windows() const noexcept { return m_windows; }

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

        // Three-member destruction order, leaning on reverse-of-
        // declaration semantics. The members below MUST stay in this
        // order:
        //
        //   1. `window` destructs first (last-declared) → every
        //      MainWindow is released → every TerminalControl
        //      detaches → every surface is freed.
        //   2. `m_ghostty` destructs → `ghostty_app_free` joins the
        //      surface / IO worker threads. In-flight callbacks fired
        //      from a thread that hasn't observed the join yet can
        //      still find `m_runtime` alive at this point.
        //   3. `m_runtime` destructs last. By now the join is done
        //      and no more callbacks can fire — safe to release the
        //      IGhosttyRuntime the factory's userdata pointer was
        //      aimed at.
        //
        // `m_windows` is a borrow-only aggregate; its own destruction
        // order relative to the members below doesn't matter (the
        // pointers don't own the MainWindows, `window` does), but
        // every MainWindow unregisters itself in its destructor which
        // runs during step 1, so by the time we reach step 2 the
        // aggregate is already empty.
        MainWindows                        m_windows;
        std::unique_ptr<MainWindowRuntime> m_runtime;
        std::unique_ptr<core::ghostty::App> m_ghostty;

        winrt::Microsoft::UI::Xaml::Window window{ nullptr };
        // Token for the primary AppInstance's Activated event; the
        // subscription lives for the lifetime of the App.
        winrt::event_token m_activatedToken{};
    };
}
