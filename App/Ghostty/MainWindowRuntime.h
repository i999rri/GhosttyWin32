#pragma once

#include "Ghostty/IGhosttyRuntime.h"
#include "Tabs/Panes/PaneId.h"

#include <functional>

namespace winrt::GhosttyWin32::implementation {

struct MainWindow;

// App-side implementation of the runtime hooks ghostty calls back
// into. The factory in Core does the C↔C++ translation; this class
// holds the bridge logic — clipboard via Win32, action dispatch
// through CallbackDispatcher, surface close via PaneId. Everything
// outside its own window domain (readiness state, ghostty ticking,
// window lookup) is passed in through the `Host` bundle so the class
// stays focused and testable.
//
// Owned by `winrt::App` as a `std::unique_ptr` member declared
// *before* the ghostty wrapper, so destruction order survives
// `ghostty_app_free`'s surface-thread join — any in-flight callback
// still sees a live runtime.
class MainWindowRuntime : public core::ghostty::IGhosttyRuntime {
public:
    // Predicate consulted at the top of every callback before host
    // state gets touched. App decides what "ready" means (which
    // statics have to be alive, whether a window is registered, …);
    // this class only knows there's a function to ask.
    using ReadinessCheck = std::function<bool()>;

    // Drives ghostty's event loop forward. OnWakeup dispatches this
    // to the UI thread. Injected instead of reaching for
    // `App::g_app->Ghostty()->Tick()` directly so this class doesn't
    // touch App scope.
    using WakeupTick = std::function<void()>;

    // Returns the window whose tab tree owns `surface`, or null when
    // none does. Called for SURFACE-target actions and for
    // clipboard / close routing that comes with a surface handle.
    using FindWindowBySurface = std::function<MainWindow*(ghostty_surface_t)>;

    // Returns any live window (currently: the first registered) or
    // null when the aggregate is empty. Called for callbacks that
    // have no per-surface handle — wakeup, clipboard read/write with
    // no explicit target, APP-target actions.
    using AnyWindow = std::function<MainWindow*()>;

    // Returns the window whose tab tree owns the given `PaneId`, or
    // null. `close_surface_cb` gives us a userdata that's a globally
    // unique PaneId (issued from the App-scope allocator), so this
    // resolves to exactly one window even with several open.
    using FindWindowByPaneId = std::function<MainWindow*(PaneId)>;

    // Bundle of callables the runtime consults. `App` fills each
    // slot at construction time. Reads at the call sites are
    // `m_host.xxx(...)`, so the runtime touches its dependencies
    // through method-like syntax while every dependency stays
    // visible on the type — adding a new slot doesn't break existing
    // callers, only requires filling one more field.
    //
    // Each closure must remain callable for the runtime's lifetime.
    // App owns both this runtime and the state the closures capture,
    // so their lifetimes are naturally tied.
    struct Host {
        ReadinessCheck      isReady;
        WakeupTick          wakeupTick;
        FindWindowBySurface findWindowBySurface;
        AnyWindow           anyWindow;
        FindWindowByPaneId  findWindowByPaneId;
    };

    explicit MainWindowRuntime(Host host);
    ~MainWindowRuntime() override = default;

    MainWindowRuntime(MainWindowRuntime const&)            = delete;
    MainWindowRuntime& operator=(MainWindowRuntime const&) = delete;

    void OnWakeup() override;
    bool OnAction(ghostty_target_s target,
                  ghostty_action_s action) override;
    bool OnReadClipboard(void* state) override;
    void OnConfirmReadClipboard(char const* content, void* state) override;
    void OnWriteClipboard(char const* utf8) override;
    void OnCloseSurface(void* paneIdUserdata) override;

private:
    Host m_host;
};

}  // namespace winrt::GhosttyWin32::implementation
