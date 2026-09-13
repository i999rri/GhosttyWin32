#include "pch.h"
#include "Ghostty/MainWindowRuntime.h"

#include "Windows/MainWindow.xaml.h"
#include "Ghostty/CallbackDispatcher.h"
#include "Interop/Encoding.h"
#include "Tabs/Panes/PaneId.h"
#include "Win32/Clipboard.h"

#include <cstring>
#include <string>
#include <vector>

namespace winrt::GhosttyWin32::implementation {

namespace {
// The Win32 clipboard is served as text only; this is the MIME type
// ghostty asks for and receives for it.
constexpr char kTextPlain[] = "text/plain";

bool IsTextPlain(char const* mime) {
    return mime && std::strcmp(mime, kTextPlain) == 0;
}
}  // namespace

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

ghostty_clipboard_read_result_e MainWindowRuntime::OnReadClipboard(
    void* paneIdUserdata,
    void* state,
    char const* const* mimes,
    size_t mimesLen,
    bool list)
{
    if (!m_host.isReady()) return GHOSTTY_CLIPBOARD_READ_UNSUPPORTED;
    auto ref = ResolvePane(paneIdUserdata);
    if (!ref.control || !ref.control->Surface()) return GHOSTTY_CLIPBOARD_READ_UNSUPPORTED;

    // Only a text/plain representation exists here, so the clipboard
    // is read when that type is requested or when the listing of
    // available types is wanted; other requested types simply have no
    // representation in the completion.
    bool wantsText = false;
    for (size_t i = 0; i < mimesLen; ++i) {
        if (IsTextPlain(mimes[i])) { wantsText = true; break; }
    }

    std::string utf8;
    if (wantsText || list) {
        utf8 = interop::Encoding::toUtf8(win32::Clipboard::read(ref.window->m_hwnd));
    }

    std::vector<ghostty_clipboard_content_s> contents;
    if (wantsText && !utf8.empty()) {
        contents.push_back({ kTextPlain, utf8.data(), utf8.size() });
    }
    std::vector<char const*> available;
    if (list && !utf8.empty()) available.push_back(kTextPlain);

    // Nothing requested is on the clipboard and no listing was asked
    // for: there is nothing to complete the read with.
    if (contents.empty() && !list) return GHOSTTY_CLIPBOARD_READ_UNAVAILABLE;

    ghostty_clipboard_complete_s const complete{
        .contents      = contents.empty() ? nullptr : contents.data(),
        .contents_len  = contents.size(),
        .available     = available.empty() ? nullptr : available.data(),
        .available_len = available.size(),
        .confirmed     = false,
        .remember      = false,
    };
    ref.control->Surface().CompleteClipboardRequest(complete, state);
    return GHOSTTY_CLIPBOARD_READ_STARTED;
}

void MainWindowRuntime::OnConfirmReadClipboard(void* paneIdUserdata,
                                               ghostty_clipboard_confirm_s const* confirm,
                                               void* state)
{
    // No permission prompt on this host yet: the read is confirmed
    // with exactly the contents ghostty offered, so the clipboard is
    // never re-read between the request and its completion.
    if (!m_host.isReady()) return;
    auto ref = ResolvePane(paneIdUserdata);
    if (!ref.control || !ref.control->Surface()) return;
    if (!confirm) {
        ref.control->Surface().DenyClipboardRequest(state);
        return;
    }

    ghostty_clipboard_complete_s const complete{
        .contents      = confirm->contents,
        .contents_len  = confirm->contents_len,
        .available     = confirm->available,
        .available_len = confirm->available_len,
        .confirmed     = true,
        .remember      = false,
    };
    ref.control->Surface().CompleteClipboardRequest(complete, state);
}

void MainWindowRuntime::OnWriteClipboard(void* paneIdUserdata,
                                         ghostty_clipboard_content_s const* contents,
                                         size_t count)
{
    if (!m_host.isReady()) return;
    auto ref = ResolvePane(paneIdUserdata);
    if (!ref.window) return;

    // The first text/plain representation goes to the Win32 clipboard.
    // The data carries an explicit length and is not null-terminated.
    for (size_t i = 0; i < count; ++i) {
        auto const& content = contents[i];
        if (!IsTextPlain(content.mime) || !content.data) continue;
        win32::Clipboard::write(
            ref.window->m_hwnd,
            interop::Encoding::toUtf16(content.data, static_cast<int>(content.len)));
        return;
    }
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
