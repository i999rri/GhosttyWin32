#include "pch.h"
#include "Ghostty/MainWindowRuntime.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Ghostty/CallbackDispatcher.h"
#include "Interop/Encoding.h"
#include "Tabs/Panes/PaneId.h"
#include "Win32/Clipboard.h"

namespace winrt::GhosttyWin32::implementation {

namespace interop = core::interop;
namespace win32   = core::win32;

// Lifecycle-defense helper. Every method opens with this — anything
// past it can assume `g_mainWindow` and `App::g_app->Ghostty()` are
// both valid for the duration of the call. Returns false during the
// brief teardown window where MainWindow has gone but the ghostty
// wrapper hasn't yet, or before the Activated handler has set
// `g_mainWindow` (the runtime exists earlier than the first window
// because App::OnLaunched creates it before make<MainWindow>()).
static bool HostIsReady() noexcept
{
    return g_mainWindow != nullptr
        && App::g_app != nullptr
        && App::g_app->Ghostty() != nullptr;
}

void MainWindowRuntime::OnWakeup()
{
    // Wakeup arrives on a worker thread. Hop to the UI thread before
    // touching ghostty, and re-check on the other side — the host
    // can transition into the teardown window between us queueing
    // and the dispatcher pulling the item.
    if (!HostIsReady()) return;
    g_mainWindow->DispatcherQueue().TryEnqueue([]() {
        if (!HostIsReady()) return;
        App::g_app->Ghostty()->Tick();
    });
}

bool MainWindowRuntime::OnAction(ghostty_target_s target,
                                  ghostty_action_s action)
{
    // Thin forwarder. All dispatch + handler bodies live in
    // GhosttyCallbackDispatcher / GhosttyActions.
    if (!HostIsReady() || !g_mainWindow->m_ghosttyDispatcher) return false;
    return g_mainWindow->m_ghosttyDispatcher->DispatchAction(target, action);
}

bool MainWindowRuntime::OnReadClipboard(void* state)
{
    if (!HostIsReady()) return false;
    auto* tc = g_mainWindow->ActiveControl();
    if (!tc || !tc->Surface()) return false;
    auto utf8 = interop::Encoding::toUtf8(
        win32::Clipboard::read(g_mainWindow->m_hwnd));
    if (utf8.empty()) return false;
    tc->Surface().CompleteClipboardRequest(utf8.c_str(), state, false);
    return true;
}

void MainWindowRuntime::OnConfirmReadClipboard(char const* content, void* state)
{
    // Auto-confirm clipboard reads.
    if (!HostIsReady()) return;
    auto* tc = g_mainWindow->ActiveControl();
    if (tc && tc->Surface()) {
        tc->Surface().CompleteClipboardRequest(content, state, true);
    }
}

void MainWindowRuntime::OnWriteClipboard(char const* utf8)
{
    if (!HostIsReady()) return;
    win32::Clipboard::write(
        g_mainWindow->m_hwnd, interop::Encoding::toUtf16(utf8));
}

void MainWindowRuntime::OnCloseSurface(void* paneIdUserdata)
{
    // Shell exited (e.g. user typed `exit`) or ghostty otherwise asked
    // to close the surface. The userdata is the PaneId we set in
    // TabFactory::MakeLeaf. Dispatch the UI mutation to the next UI
    // tick so it happens off the renderer thread.
    if (!HostIsReady()) return;
    PaneId id = PaneId::FromUserdata(paneIdUserdata);
    auto* mw = g_mainWindow;
    mw->DispatcherQueue().TryEnqueue([mw, id]() {
        mw->CloseSurfaceByPaneId(id);
    });
}

}  // namespace winrt::GhosttyWin32::implementation
