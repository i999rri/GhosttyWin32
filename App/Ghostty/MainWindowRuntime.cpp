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

MainWindowRuntime::PaneRef
MainWindowRuntime::ResolvePane(void* paneIdUserdata) const
{
    // The clipboard callbacks carry the same per-surface userdata as
    // close_surface: the PaneId set in TabFactory::MakeLeaf. Globally
    // unique (App-scope allocator), so this resolves to exactly one
    // window / control even with several windows open. Null results
    // mean the pane died before the callback landed — callers no-op.
    PaneRef ref;
    PaneId id = PaneId::FromUserdata(paneIdUserdata);
    if (!id) return ref;
    ref.window = m_host.findWindowByPaneId(id);
    if (!ref.window) return ref;
    ref.control = ref.window->ControlByPaneId(id);
    return ref;
}

bool MainWindowRuntime::OnReadClipboard(void* paneIdUserdata, void* state)
{
    if (!m_host.isReady()) return false;
    auto ref = ResolvePane(paneIdUserdata);
    if (!ref.control || !ref.control->Surface()) return false;
    auto utf8 = interop::Encoding::toUtf8(
        win32::Clipboard::read(ref.window->m_hwnd));
    if (utf8.empty()) return false;
    ref.control->Surface().CompleteClipboardRequest(utf8.c_str(), state, false);
    return true;
}

void MainWindowRuntime::OnConfirmReadClipboard(void* paneIdUserdata,
                                               char const* content,
                                               void* state)
{
    // Auto-confirm clipboard reads.
    if (!m_host.isReady()) return;
    auto ref = ResolvePane(paneIdUserdata);
    if (ref.control && ref.control->Surface()) {
        ref.control->Surface().CompleteClipboardRequest(content, state, true);
    }
}

void MainWindowRuntime::OnWriteClipboard(void* paneIdUserdata, char const* utf8)
{
    if (!m_host.isReady()) return;
    auto ref = ResolvePane(paneIdUserdata);
    if (!ref.window) return;
    win32::Clipboard::write(
        ref.window->m_hwnd, interop::Encoding::toUtf16(utf8));
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
