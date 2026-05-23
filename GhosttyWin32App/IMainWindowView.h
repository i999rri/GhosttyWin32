#pragma once

#include <winrt/Microsoft.UI.Dispatching.h>
#include <windows.h>

namespace winrt::GhosttyWin32::implementation {

// Narrow view-side surface that ActionDispatcher depends on. Lets
// the dispatcher reach the bits of MainWindow it actually needs
// (HWND, UI dispatcher, ghostty tick) without taking a hard
// dependency on the full MainWindow type — keeps the dispatcher
// testable in isolation and stops it from quietly accreting more
// MainWindow internals than it really needs.
//
// Grown lazily on purpose: every handler that's lifted out of
// MainWindow::action_cb adds the one or two methods here that
// it actually uses. The narrow surface is the design value; a
// full-MainWindow facade would defeat the point of the split.
struct IMainWindowView {
    virtual ~IMainWindowView() = default;

    // Top-level HWND for the host window. Borrowed; valid for the
    // lifetime of the view. Used for ShellExecuteW parenting and
    // any Win32 message-pump round-trip a handler needs.
    virtual HWND Hwnd() const noexcept = 0;

    // UI thread DispatcherQueue. action_cb fires from ghostty's
    // renderer thread, so any WinUI / DComp mutation has to bounce
    // through here before touching the visual tree.
    virtual winrt::Microsoft::UI::Dispatching::DispatcherQueue Dispatcher() const = 0;

    // Forwarder to GhosttyApp::Tick. The underlying ghostty_app_tick
    // call is wrapped with the SEH guard / NVIDIA presenter workaround
    // from issue #26, so handlers should not be tempted to invoke
    // ghostty_app_tick directly.
    virtual void Tick() = 0;

    // Tear the window down. Named "Request" because the WinUI
    // Window::Close already takes that slot in the projection and
    // can't be marked override; this thin wrapper swallows the
    // hresult_error WinUI throws when the window is already torn
    // down, so handlers can call it unconditionally.
    virtual void RequestClose() = 0;
};

}  // namespace winrt::GhosttyWin32::implementation
