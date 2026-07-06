#include "pch.h"
#include "Ghostty/MainWindowRuntime.h"

#include "MainWindow.xaml.h"
#include "Ghostty/CallbackDispatcher.h"
#include "Interop/Encoding.h"
#include "Tabs/Panes/PaneId.h"
#include "Win32/Clipboard.h"

namespace winrt::GhosttyWin32::implementation {

namespace interop = core::interop;
namespace win32   = core::win32;

MainWindowRuntime::MainWindowRuntime(ReadinessCheck isHostReady,
                                     WakeupTick     wakeupTick)
    : m_isHostReady(std::move(isHostReady))
    , m_wakeupTick(std::move(wakeupTick))
{
}

void MainWindowRuntime::OnWakeup()
{
    // Wakeup arrives on a worker thread. Hop to the UI thread before
    // touching ghostty, and re-check on the other side — the host
    // can transition out of the ready state between us queueing and
    // the dispatcher pulling the item. `[this]` captures the runtime
    // pointer; runtime lifetime outlasts ghostty per App's member
    // ordering, so the capture stays valid.
    if (!m_isHostReady()) return;
    g_mainWindow->DispatcherQueue().TryEnqueue([this]() {
        if (m_isHostReady()) m_wakeupTick();
    });
}

bool MainWindowRuntime::OnAction(ghostty_target_s target,
                                  ghostty_action_s action)
{
    // Thin forwarder. All dispatch + handler bodies live in
    // GhosttyCallbackDispatcher / GhosttyActions.
    if (!m_isHostReady() || !g_mainWindow->m_ghosttyDispatcher) return false;
    return g_mainWindow->m_ghosttyDispatcher->DispatchAction(target, action);
}

bool MainWindowRuntime::OnReadClipboard(void* state)
{
    if (!m_isHostReady()) return false;
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
    if (!m_isHostReady()) return;
    auto* tc = g_mainWindow->ActiveControl();
    if (tc && tc->Surface()) {
        tc->Surface().CompleteClipboardRequest(content, state, true);
    }
}

void MainWindowRuntime::OnWriteClipboard(char const* utf8)
{
    if (!m_isHostReady()) return;
    win32::Clipboard::write(
        g_mainWindow->m_hwnd, interop::Encoding::toUtf16(utf8));
}

void MainWindowRuntime::OnCloseSurface(void* paneIdUserdata)
{
    // Shell exited (e.g. user typed `exit`) or ghostty otherwise asked
    // to close the surface. The userdata is the PaneId we set in
    // TabFactory::MakeLeaf. Dispatch the UI mutation to the next UI
    // tick so it happens off the renderer thread.
    if (!m_isHostReady()) return;
    PaneId id = PaneId::FromUserdata(paneIdUserdata);
    auto* mw = g_mainWindow;
    mw->DispatcherQueue().TryEnqueue([mw, id]() {
        mw->CloseSurfaceByPaneId(id);
    });
}

}  // namespace winrt::GhosttyWin32::implementation
