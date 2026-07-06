#pragma once

#include "Ghostty/IGhosttyRuntime.h"

#include <functional>

namespace winrt::GhosttyWin32::implementation {

// App-side implementation of the runtime hooks ghostty calls back
// into. The factory in Core does the C↔C++ translation; this class
// holds all the bridge logic — clipboard via Win32, action dispatch
// through CallbackDispatcher, surface close via PaneId — and is the
// only thing that knows about MainWindow / App::g_app statics in
// this layer.
//
// Owned by `winrt::App` as a `std::unique_ptr` member declared
// *before* the ghostty wrapper, so that destruction order survives
// `ghostty_app_free`'s surface-thread join — any in-flight callback
// still sees a live runtime.
class MainWindowRuntime : public core::ghostty::IGhosttyRuntime {
public:
    // Predicate consulted at the top of every callback before host
    // state gets touched. Whether "ready" means "g_mainWindow set +
    // App::g_app live + ghostty wrapper alive," some subset, or a
    // completely different condition (e.g. under test) is App's
    // choice — this class only knows there's a function to ask.
    using ReadinessCheck = std::function<bool()>;

    // Drives ghostty's event loop forward. OnWakeup dispatches this
    // to the UI thread. Injected instead of calling the App-scope
    // `App::g_app->Ghostty()->Tick()` directly so this class doesn't
    // reach out of its window domain — `App` and the ghostty wrapper
    // are somebody else's concerns.
    using WakeupTick = std::function<void()>;

    // `isHostReady` and `wakeupTick` must remain callable for the
    // runtime's lifetime — App owns both this runtime and the state
    // the closures capture, so their lifetimes are naturally tied.
    MainWindowRuntime(ReadinessCheck isHostReady,
                      WakeupTick     wakeupTick);
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
    ReadinessCheck m_isHostReady;
    WakeupTick     m_wakeupTick;
};

}  // namespace winrt::GhosttyWin32::implementation
