#pragma once

#include "App.xaml.g.h"
#include "Ghostty/App.h"
#include "Windows/MainWindows.h"
#include "Windows/WindowState.h"
#include "Tabs/Panes/PaneIdAllocator.h"
#include "Tabs/PressedTab.h"
#include "Tabs/TabDrag.h"
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

        // Best-effort cleanup invoked from SetUnhandledExceptionFilter
        // when a SEH exception unwinds past the top of the call stack.
        // Process-wide by definition — walks every registered MainWindow
        // via the App-scope aggregate to release composition handles
        // before the process dies. Registered once in OnLaunched;
        // returns EXCEPTION_CONTINUE_SEARCH so the debugger / WER sees
        // the exception normally.
        static long __stdcall OnUnhandledException(struct _EXCEPTION_POINTERS* info) noexcept;

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

        // Process-wide PaneId allocator. Every leaf across every
        // MainWindow draws its id from here so PaneIds are globally
        // unique — that's what lets close_surface_cb route a
        // ghostty-side close request to the exact pane by id alone,
        // even when a second (or Nth) MainWindow enters the picture.
        // Also what `AppNotification`'s `surfaceId=` argument encodes
        // when a background notification click needs to focus a
        // specific pane regardless of which window owns it.
        PaneIdAllocator& PaneIds() noexcept { return m_paneIds; }

        // Spawns a fresh top-level MainWindow, plugs it into the
        // aggregate (via its Activated handler) and shows it. Both
        // `OnLaunched`'s initial window and future `NEW_WINDOW`
        // action handlers go through here — one path, one lifetime
        // story. A Closed subscription drops the strong reference
        // when the user closes the window; the vector doesn't hold
        // stale entries for windows that outlive their HWND.
        void CreateNewWindow();

        // Close every live top-level window (CLOSE_ALL_WINDOWS
        // action). Iterates a snapshot because each Close() erases
        // its own vector entry through the Closed subscription.
        void CloseAllWindows();

        // QUIT action. Today identical in effect to
        // CloseAllWindows() — process lifetime is tied to live
        // windows — but kept as a distinct entry point so
        // quit-specific behaviour (confirmation, session save) has
        // a home when it arrives.
        void Quit();

        // Spawn the window that will host a torn-out tab, starting
        // from `inherited` (the source window's State()). Same
        // tracking as CreateNewWindow, but with no initial tab (it
        // adopts the dropped one) and no Activate() — the drop
        // handler positions the window at the drop point after
        // adopting the tab and decides activation itself.
        MainWindow* CreateTearOutWindow(WindowState::Inherited const& inherited);

        // The live window whose tab strip owns `item`, or null.
        // Locates the source window of a dragged tab on the drop
        // paths (the drop target only receives the TabViewItem).
        MainWindow* FindWindowByTabItem(
            Microsoft::UI::Xaml::Controls::TabViewItem const& item) noexcept;

        // GOTO_WINDOW: bring the previous / next top-level window
        // forward relative to the currently-foreground one. No-op
        // when only one window exists. UI thread only.
        void PresentWindow(ghostty_action_goto_window_e direction);

        // The tab drag currently (or most recently) in flight.
        // Cross-window drag-and-drop rides OLE, which only marshals
        // primitive DataPackage values — a TabViewItem stuffed into
        // the package's Properties comes out empty on another
        // window's TabStripDrop. Every window shares this process,
        // so the dragged item travels through this object instead.
        // Lifecycle and the two-lifetime design live on BasicTabDrag.
        BasicTabDrag<Microsoft::UI::Xaml::Controls::TabViewItem>&
        TabDrag() noexcept { return m_tabDrag; }

        // App-scope like TabDrag: the press handler travels with the
        // TabViewItem across tear-out windows, so the slot it writes
        // must not belong to any one window.
        BasicPressedTab<Microsoft::UI::Xaml::Controls::TabViewItem>&
        PressedTab() noexcept { return m_pressedTab; }

    private:
        // Shared tail of CreateNewWindow / CreateTearOutWindow:
        // strong-ref the window in m_topLevelWindows and subscribe
        // the Closed auto-erase.
        void TrackWindow(Microsoft::UI::Xaml::Window const& w);

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
        //   1. `m_topLevelWindows` destructs first (last-declared) →
        //      every Window handle releases → every MainWindow
        //      destructor runs → every TerminalControl detaches →
        //      every surface is freed.
        //   2. `m_ghostty` destructs → `ghostty_app_free` joins the
        //      surface / IO worker threads. In-flight callbacks fired
        //      from a thread that hasn't observed the join yet can
        //      still find `m_runtime` alive at this point.
        //   3. `m_runtime` destructs last. By now the join is done
        //      and no more callbacks can fire — safe to release the
        //      IGhosttyRuntime the factory's userdata pointer was
        //      aimed at.
        //
        // Process-wide PaneId issuer. Held by App because MainWindow
        // ctor / tab creation both need it before their own state is
        // fully assembled; sharing the counter across windows keeps
        // ids collision-free for close_surface_cb routing.
        PaneIdAllocator                    m_paneIds;
        // `m_windows` is a borrow-only aggregate; its own destruction
        // order relative to the members below doesn't matter (the
        // pointers don't own the MainWindows, `m_topLevelWindows`
        // does), but every MainWindow unregisters itself in its
        // destructor which runs during step 1, so by the time we
        // reach step 2 the aggregate is already empty.
        MainWindows                        m_windows;
        std::unique_ptr<MainWindowRuntime> m_runtime;
        std::unique_ptr<core::ghostty::App> m_ghostty;

        // Strong references to every top-level window we've spawned.
        // The vector owns each `Window` handle (the WinRT smart
        // pointer form) until the user closes that window; a
        // per-window Closed subscription installed by CreateNewWindow
        // erases the matching entry so we don't stack up dangling
        // handles across a long session. On App teardown, whatever's
        // still here destructs during m_topLevelWindows' own
        // destruction and each MainWindow unregisters itself from
        // the aggregate above as part of the same unwind.
        std::vector<winrt::Microsoft::UI::Xaml::Window> m_topLevelWindows;
        // Token for the primary AppInstance's Activated event; the
        // subscription lives for the lifetime of the App.
        winrt::event_token m_activatedToken{};
        BasicTabDrag<Microsoft::UI::Xaml::Controls::TabViewItem> m_tabDrag;
        BasicPressedTab<Microsoft::UI::Xaml::Controls::TabViewItem> m_pressedTab;
    };
}
