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

MainWindowRuntime::MainWindowRuntime(Host host)
    : m_host(std::move(host))
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
    if (!m_host.isReady()) return;
    auto* window = m_host.anyWindow();
    if (!window) return;
    window->DispatcherQueue().TryEnqueue([this]() {
        if (m_host.isReady()) m_host.wakeupTick();
    });
}

bool MainWindowRuntime::OnAction(ghostty_target_s target,
                                  ghostty_action_s action)
{
    // Thin forwarder. All dispatch + handler bodies live in
    // GhosttyCallbackDispatcher / GhosttyActions.
    if (!m_host.isReady()) return false;
    auto* window = (target.tag == GHOSTTY_TARGET_SURFACE)
        ? m_host.findWindowBySurface(target.target.surface)
        : m_host.anyWindow();
    if (!window || !window->m_ghosttyDispatcher) return false;
    return window->m_ghosttyDispatcher->DispatchAction(target, action);
}

bool MainWindowRuntime::OnReadClipboard(void* state)
{
    if (!m_host.isReady()) return false;
    auto* window = m_host.anyWindow();
    if (!window) return false;
    auto* tc = window->ActiveControl();
    if (!tc || !tc->Surface()) return false;
    auto utf8 = interop::Encoding::toUtf8(
        win32::Clipboard::read(window->m_hwnd));
    if (utf8.empty()) return false;
    tc->Surface().CompleteClipboardRequest(utf8.c_str(), state, false);
    return true;
}

void MainWindowRuntime::OnConfirmReadClipboard(char const* content, void* state)
{
    // Auto-confirm clipboard reads.
    if (!m_host.isReady()) return;
    auto* window = m_host.anyWindow();
    if (!window) return;
    auto* tc = window->ActiveControl();
    if (tc && tc->Surface()) {
        tc->Surface().CompleteClipboardRequest(content, state, true);
    }
}

void MainWindowRuntime::OnWriteClipboard(char const* utf8)
{
    if (!m_host.isReady()) return;
    auto* window = m_host.anyWindow();
    if (!window) return;
    win32::Clipboard::write(
        window->m_hwnd, interop::Encoding::toUtf16(utf8));
}

void MainWindowRuntime::OnCloseSurface(void* paneIdUserdata)
{
    // Shell exited (e.g. user typed `exit`) or ghostty otherwise asked
    // to close the surface. The userdata is the PaneId we set in
    // TabFactory::MakeLeaf — a globally unique id, so resolving to
    // the owning window is a lookup rather than "assume the only
    // window." Dispatch the UI mutation to the next UI tick so it
    // happens off the renderer thread.
    if (!m_host.isReady()) return;
    PaneId id = PaneId::FromUserdata(paneIdUserdata);
    auto* window = m_host.findWindowByPaneId(id);
    if (!window) return;
    window->DispatcherQueue().TryEnqueue([window, id]() {
        window->CloseSurfaceByPaneId(id);
    });
}

}  // namespace winrt::GhosttyWin32::implementation
