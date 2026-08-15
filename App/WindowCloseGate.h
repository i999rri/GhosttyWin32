#pragma once

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <functional>
#include <utility>

namespace winrt::GhosttyWin32::implementation {

// Value type describing what a user wants closed. Passed through the
// gate as a single argument so entry points (custom X button, tab-header
// X, close_surface keybind, WM_CLOSE subclass) fund into the same
// RequestClose signature — they just build a different scope.
//
// Two constructors, two kinds:
//   * Window: the whole top-level MainWindow (all tabs, all panes).
//   * Tab(item): one tab identified by its TabViewItem. That item is the
//     stable identity the TabView already uses for lookups; passing it
//     through avoids re-resolving the target inside the gate's Ops.
class CloseScope {
public:
    enum class Kind { Window, Tab };

    static CloseScope Window() noexcept {
        return CloseScope{Kind::Window, nullptr};
    }
    static CloseScope Tab(
        Microsoft::UI::Xaml::Controls::TabViewItem item) noexcept
    {
        return CloseScope{Kind::Tab, std::move(item)};
    }

    Kind kind() const noexcept { return m_kind; }
    Microsoft::UI::Xaml::Controls::TabViewItem const& tabItem() const noexcept {
        return m_item;
    }

private:
    CloseScope(Kind k,
               Microsoft::UI::Xaml::Controls::TabViewItem item) noexcept
        : m_kind(k), m_item(std::move(item)) {}

    Kind m_kind;
    Microsoft::UI::Xaml::Controls::TabViewItem m_item{ nullptr };
};

// State machine gating window-close flow. Owns the "dialog in flight" and
// "window close in progress" invariants so every close entry point routes
// through one place instead of scattering fields across MainWindow.
//
// Injected Ops thin the class down to state transitions + policy
// delegation: MainWindow supplies how to check "does this scope need
// confirmation?", how to render the dialog, and how to execute the
// actual close. The state machine itself is pure — a std::function
// fake for Ops exercises every transition (approve, cancel, in-flight
// guard, re-entrance from close-triggered WM_CLOSE) without WinUI /
// Win32.
//
// Invariants protected:
//
//   1. At most one confirmation dialog is on screen at a time. Rapid
//      Alt+F4 / X clicks land in Prompting and are ignored until the
//      current dialog resolves.
//   2. Close only proceeds after confirmation OR when confirmation
//      isn't required (needsConfirm returns false).
//   3. Cancel returns cleanly to Idle — no half-torn state.
//   4. Once a Window-scope close has been approved, WindowCloseInProgress
//      stays true so the WM_CLOSE subclass can distinguish "internal
//      close following approval" (default-proceed) from "user's fresh
//      Alt+F4" (re-enter the gate).
class WindowCloseGate {
public:
    struct Ops {
        // Synchronous predicate: given a scope, does any surface inside
        // need confirmation right now? Called on every RequestClose.
        std::function<bool(CloseScope const&)> needsConfirm;

        // Async dialog. Gate hands over a decision callback that must
        // fire exactly once with `true` (user approved) or `false`
        // (cancelled / dismissed). The callback may fire on a later
        // dispatcher turn — the gate is safe against that.
        std::function<void(CloseScope const&,
                           std::function<void(bool)> onDecision)> showDialog;

        // Perform the actual close. For Window scope, take the top-level
        // window down (typically Window::Close). For Tab scope, remove
        // that tab from the TabView and clean up its panes.
        std::function<void(CloseScope const&)> execute;
    };

    explicit WindowCloseGate(Ops ops) noexcept : m_ops(std::move(ops)) {}

    // Entry point. Every user-facing close signal (X, keybind, WM_CLOSE)
    // reaches the gate through here. Guarantees the four invariants
    // documented on the class.
    void RequestClose(CloseScope scope) {
        // Invariant 1: dialog already up, second signal is noise.
        if (m_state == State::Prompting) return;

        // Invariant 4: after a Window close is approved, the window is
        // tearing down. Further requests during that teardown flow are
        // internal (the Close() call re-enters WM_CLOSE) and shouldn't
        // re-prompt.
        if (m_windowCloseInProgress) return;

        // Fresh request. Consult policy.
        if (!m_ops.needsConfirm(scope)) {
            markInProgressIfWindow(scope);
            m_ops.execute(scope);
            return;
        }

        // Confirmation required. Hand off to async dialog; decision
        // callback drives back to Idle (cancel) or Approved+execute
        // (Primary).
        m_state = State::Prompting;
        m_ops.showDialog(scope,
            [this, scope](bool approved) mutable {
                m_state = State::Idle;
                if (!approved) return;
                markInProgressIfWindow(scope);
                m_ops.execute(scope);
            });
    }

    // Read by the WM_CLOSE subclass: is a Window-scope close currently
    // executing? True after approve (or after no-confirm-needed) for a
    // Window scope, stays true — the window is dying, nothing else in
    // this gate ever runs again.
    bool WindowCloseInProgress() const noexcept {
        return m_windowCloseInProgress;
    }

private:
    void markInProgressIfWindow(CloseScope const& scope) noexcept {
        if (scope.kind() == CloseScope::Kind::Window) {
            m_windowCloseInProgress = true;
        }
    }

    enum class State { Idle, Prompting };
    State m_state{ State::Idle };
    bool  m_windowCloseInProgress{ false };
    Ops   m_ops;
};

}  // namespace winrt::GhosttyWin32::implementation
