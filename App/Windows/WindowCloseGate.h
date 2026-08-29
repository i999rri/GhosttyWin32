#pragma once

#include <winrt/Microsoft.UI.Xaml.h>
#include <functional>
#include <memory>

namespace winrt::GhosttyWin32::implementation {

// Single point that every close intent inside one MainWindow flows
// through. Turns the "should we prompt?" decision into a state — no
// dialog in flight, or one dialog for one scope — so overlapping
// close requests can't stack ContentDialogs on top of each other.
//
// The gate owns no ghostty state; it only holds the XamlRoot (needed
// to anchor the dialog) and the in-flight flag. Callers pass the
// scope-aware needs-confirm predicate and the "actually close" action
// as callbacks — the gate never reaches into MainWindow directly, so
// the close paths still express their own teardown ordering.
class WindowCloseGate {
public:
    // What is being closed. Chooses the dialog wording and — via the
    // caller-provided predicate — which panes get walked for
    // needs_confirm_quit.
    enum class Scope {
        Surface,   // single pane (close_surface action)
        Tab,       // one tab   (tab X, close_tab action)
        Window,    // whole window (titlebar X, Alt+F4)
    };

    WindowCloseGate() noexcept = default;
    WindowCloseGate(WindowCloseGate const&) = delete;
    WindowCloseGate& operator=(WindowCloseGate const&) = delete;
    WindowCloseGate(WindowCloseGate&&) = delete;
    WindowCloseGate& operator=(WindowCloseGate&&) = delete;

    bool IsIdle()    const noexcept { return !*m_pending; }
    bool IsPending() const noexcept { return  *m_pending; }

    // Enter the gate. On the UI thread. If `needsConfirm()` returns
    // false the gate fires `onApproved` synchronously and stays Idle;
    // otherwise it shows the ContentDialog, transitions to Pending,
    // and fires `onApproved` from the dialog's Completed handler on
    // the OK path. Requests submitted while Pending are dropped so
    // dialogs don't stack. `xamlRoot` is the anchor for the dialog —
    // passed per-call because MainWindow.Content().XamlRoot() only
    // becomes available once the tree is realised, and re-reading
    // it each time avoids caching a stale root across teardown.
    void Submit(Scope scope,
                winrt::Microsoft::UI::Xaml::XamlRoot xamlRoot,
                std::function<bool()> needsConfirm,
                std::function<void()> onApproved);

private:
    // Heap-owned so the dialog's Completed lambda can reset it via
    // a shared_ptr copy — capturing `this` would dangle if the gate
    // (owned by MainWindow) is destroyed while the dialog is still
    // in flight.
    std::shared_ptr<bool> m_pending{ std::make_shared<bool>(false) };
};

}  // namespace winrt::GhosttyWin32::implementation
