#pragma once

#include "Ghostty/IGhosttyRuntime.h"

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
    MainWindowRuntime() = default;
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
};

}  // namespace winrt::GhosttyWin32::implementation
