#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Ghostty/MainWindowRuntime.h"
#include "Ghostty/RuntimeConfigFactory.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Microsoft.Windows.AppNotifications.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace {
    // Flag file used to detect that the previous process didn't exit
    // cleanly. Created in OnLaunched, removed in ~App on clean
    // shutdown — if it's still there at next launch, the previous
    // run crashed and OnLaunched waits briefly so the NVIDIA driver
    // has time to recover its internal state.
    std::filesystem::path crashFlagPath() {
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

    // Out-of-line so MainWindowRuntime's full definition (included
    // above) is in scope where the unique_ptr destructor is
    // instantiated. The body itself is empty — destruction runs
    // member-by-member per the App.xaml.h declaration order.
    App::~App()
    {
        // Clean shutdown reached — remove the crash flag so the next
        // launch doesn't sit through the 2-second driver recovery pause.
        // Best-effort: if the filesystem call fails we lose nothing
        // beyond the extra pause on the next start.
        std::error_code ec;
        std::filesystem::remove(crashFlagPath(), ec);
    }

    long __stdcall App::OnUnhandledException(struct _EXCEPTION_POINTERS* /*info*/) noexcept
    {
        // Fatal crash inside the process. Best-effort cleanup: hide
        // every registered window and release each active
        // TerminalControl's composition handle before letting
        // WER / debugger take over. We deliberately don't touch XAML
        // objects (releasing the surfaces via DComp is enough) or
        // ghostty structures that might be wrecked. If any of them
        // does crash anyway, the unhandled-exception filter "fails"
        // recursively and WER takes over with its standard dialog —
        // same end result, just less polished. That's an acceptable
        // trade for keeping this code readable.
        OutputDebugStringA("GhosttyWin32: unhandled exception, attempting cleanup\n");
        if (g_app) {
            // Walk every registered window; the SEH handler is
            // process-wide, and once multi-window lands a fatal
            // crash still wants every window's composition handles
            // released before we hand control back.
            for (auto* w : g_app->Windows()) {
                if (!w) continue;
                if (w->m_hwnd) ShowWindow(w->m_hwnd, SW_HIDE);
                for (auto& tab : w->m_tabs) {
                    if (!tab) continue;
                    if (auto* tc = tab->ActiveControl()) {
                        HANDLE h = tc->CompositionHandle();
                        if (h) CloseHandle(h);
                    }
                }
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

    void App::OnInstanceActivated(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments const& args)
    {
        // Activated fires on a worker — bounce to the UI thread before
        // touching any Xaml state. If the redirect happened before
        // OnLaunched added the first window, just drop it; OnLaunched
        // will run normally and present the terminal as a first-launch
        // effect.
        if (m_topLevelWindows.empty()) return;
        m_topLevelWindows.front().DispatcherQueue().TryEnqueue(
            [weak = get_weak(), args]() {
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
        if (auto* mw = m_windows.Any()) {
            mw->PresentNotification(implementation::PaneId{ 0 });
        }
    }

    void App::RouteNotificationClick(uint64_t paneIdValue)
    {
        // Called from the wWinMain-side NotificationInvoked subscriber
        // on a worker thread. Bounce to the UI thread before touching
        // any Xaml state. If the click arrives before OnLaunched has
        // added the first window, drop it — the user just gets the
        // regular foreground from AppInstance::Activated.
        if (m_topLevelWindows.empty()) return;
        m_topLevelWindows.front().DispatcherQueue().TryEnqueue(
            [weak = get_weak(), paneIdValue]()
            {
                auto self = weak.get();
                if (!self) return;
                // Route the click to the specific window that owns
                // the targeted pane; fall back to any live window if
                // the id is the "no target" sentinel or the owning
                // window has since closed.
                PaneId target{ paneIdValue };
                MainWindow* mw = target
                    ? self->m_windows.FindForPaneId(target)
                    : nullptr;
                if (!mw) mw = self->m_windows.Any();
                if (mw) {
                    mw->PresentNotification(target);
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
            auto flag = crashFlagPath();
            if (std::filesystem::exists(flag, ec)) {
                OutputDebugStringA("GhosttyWin32: previous run crashed; pausing 2s for driver recovery\n");
                Sleep(2000);
            }
            std::ofstream(flag).close();
        }

        // Register the process-wide SEH handler now that the crash
        // flag is in place. Historically this lived in MainWindow's
        // Activated handler; moving it here makes it independent of
        // window count — a fatal exception with two windows open
        // still walks every window's composition handles before we
        // let WER take over.
        SetUnhandledExceptionFilter(&App::OnUnhandledException);

        // AppNotificationManager::Default().Register() and the
        // NotificationInvoked subscription both already happened in
        // wWinMain (see main.cpp). Windows App SDK 1.8 throws and
        // fail-fasts if we try to re-subscribe NotificationInvoked
        // here after Register(), so the routing path goes via the
        // wWinMain-side subscriber calling App::RouteNotificationClick.

        // Bring up ghostty BEFORE constructing the window. The
        // runtime is created first, then handed to the factory as
        // the rtConfig userdata — every C callback ghostty fires
        // unwraps that pointer back to our IGhosttyRuntime impl. The
        // first callback won't fire until a surface is created, by
        // which point Activate has run and the Activated handler has
        // registered the new MainWindow with the aggregate, so
        // MainWindowRuntime's lookups return non-null.
        //
        // Member ordering in App.xaml.h enforces destruction order
        // (window → m_ghostty → m_runtime), keeping the userdata
        // pointer alive across ghostty_app_free's surface-thread
        // join.
        // The runtime consults these callables through its Host
        // bundle. Keeping App-scope knowledge here — which state has
        // to be alive, how to reach the ghostty wrapper, how to look
        // up windows — means MainWindowRuntime doesn't hard-code any
        // of it. designated-initialiser field names double as
        // documentation for what each closure does.
        m_runtime = std::make_unique<MainWindowRuntime>(MainWindowRuntime::Host{
            .isReady = []() {
                return App::g_app != nullptr
                    && !App::g_app->Windows().Empty()
                    && App::g_app->Ghostty() != nullptr;
            },
            .wakeupTick = []() { App::g_app->Ghostty()->Tick(); },
            .findWindowBySurface = [](ghostty_surface_t s) -> MainWindow* {
                return App::g_app->Windows().FindForSurface(s);
            },
            .anyWindow = []() -> MainWindow* {
                return App::g_app->Windows().Any();
            },
            .findWindowByPaneId = [](PaneId id) -> MainWindow* {
                return App::g_app->Windows().FindForPaneId(id);
            },
        });
        m_ghostty = core::ghostty::App::Create(
            core::ghostty::RuntimeConfigFactory::Build(m_runtime.get()));
        if (!m_ghostty) {
            OutputDebugStringW(L"[App] ghostty init failed; not creating window\n");
            return;
        }

        CreateNewWindow();
    }

    void App::CreateNewWindow()
    {
        auto w = make<MainWindow>();
        m_topLevelWindows.push_back(w);
        // Auto-erase the vector entry when the user closes the
        // window. Capture only `this`; the sender comes in through
        // the event args, so no strong Window reference is trapped
        // inside the lambda — closing really is the last thing that
        // keeps the Window alive.
        w.Closed([this](winrt::Windows::Foundation::IInspectable const& sender,
                        winrt::Microsoft::UI::Xaml::WindowEventArgs const&) {
            auto closing = sender.try_as<winrt::Microsoft::UI::Xaml::Window>();
            if (!closing) return;
            auto it = std::find(m_topLevelWindows.begin(),
                                m_topLevelWindows.end(),
                                closing);
            if (it != m_topLevelWindows.end()) {
                m_topLevelWindows.erase(it);
            }
        });
        w.Activate();
    }
}
