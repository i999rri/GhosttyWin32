#include "pch.h"
#include "Ghostty/RuntimeConfigFactory.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Ghostty/CallbackDispatcher.h"
#include "Interop/Encoding.h"
#include "Tabs/Panes/PaneId.h"
#include "Win32/Clipboard.h"

namespace winrt::GhosttyWin32::implementation {

namespace interop = core::interop;
namespace win32   = core::win32;

ghostty_runtime_config_s RuntimeConfigFactory::Build()
{
    ghostty_runtime_config_s rtConfig{};
    rtConfig.userdata                  = nullptr;
    rtConfig.wakeup_cb                 = &Wakeup;
    rtConfig.action_cb                 = &Action;
    rtConfig.read_clipboard_cb         = &ReadClipboard;
    rtConfig.confirm_read_clipboard_cb = &ConfirmReadClipboard;
    rtConfig.write_clipboard_cb        = &WriteClipboard;
    rtConfig.close_surface_cb          = &CloseSurface;
    return rtConfig;
}

void RuntimeConfigFactory::Wakeup(void*)
{
    // Wakeup arrives on a worker thread. Hop to the UI thread before
    // touching ghostty, and re-check `App::Ghostty()` there —
    // `App::g_app` may still be live during late shutdown but the
    // ghostty wrapper itself can already be gone.
    if (!g_mainWindow || !App::g_app || !App::g_app->Ghostty()) return;
    g_mainWindow->DispatcherQueue().TryEnqueue([]() {
        if (App::g_app) {
            if (auto* g = App::g_app->Ghostty()) g->Tick();
        }
    });
}

bool RuntimeConfigFactory::Action(ghostty_app_t,
                                  ghostty_target_s target,
                                  ghostty_action_s action)
{
    // Thin forwarder. All dispatch + handler bodies live in
    // GhosttyCallbackDispatcher / GhosttyActions; this method only
    // exists because ghostty's runtime config wants a C function
    // pointer.
    if (!g_mainWindow || !g_mainWindow->m_ghosttyDispatcher) return false;
    return g_mainWindow->m_ghosttyDispatcher->DispatchAction(target, action);
}

bool RuntimeConfigFactory::ReadClipboard(void*,
                                         ghostty_clipboard_e,
                                         void* state)
{
    if (!g_mainWindow) return false;
    auto* tc = g_mainWindow->ActiveControl();
    if (!tc || !tc->Surface()) return false;
    auto utf8 = interop::Encoding::toUtf8(
        win32::Clipboard::read(g_mainWindow->m_hwnd));
    if (utf8.empty()) return false;
    tc->Surface().CompleteClipboardRequest(utf8.c_str(), state, false);
    return true;
}

void RuntimeConfigFactory::ConfirmReadClipboard(void*,
                                                char const* content,
                                                void* state,
                                                ghostty_clipboard_request_e)
{
    // Auto-confirm clipboard reads.
    if (!g_mainWindow) return;
    auto* tc = g_mainWindow->ActiveControl();
    if (tc && tc->Surface()) {
        tc->Surface().CompleteClipboardRequest(content, state, true);
    }
}

void RuntimeConfigFactory::WriteClipboard(void*,
                                          ghostty_clipboard_e,
                                          ghostty_clipboard_content_s const* content,
                                          size_t count,
                                          bool)
{
    if (!content || count == 0 || !content[0].data) return;
    HWND hwnd = g_mainWindow ? g_mainWindow->m_hwnd : nullptr;
    win32::Clipboard::write(hwnd, interop::Encoding::toUtf16(content[0].data));
}

void RuntimeConfigFactory::CloseSurface(void* userdata, bool /*process_alive*/)
{
    // Shell exited (e.g. user typed `exit`) or ghostty otherwise asked
    // to close the surface. The userdata is the PaneId we set in
    // TabFactory::MakeLeaf. Dispatch the UI mutation to the next UI
    // tick so it happens off the renderer thread.
    //
    // Two cases:
    //   * Leaf is the only pane in its tab → close the tab.
    //   * Leaf has a sibling → collapse the split. The surviving
    //     sibling takes the parent split's slot; if the closed pane
    //     was the active leaf, focus moves to the first leaf under
    //     the surviving subtree.
    if (!g_mainWindow || !userdata) return;
    PaneId id = PaneId::FromUserdata(userdata);
    auto mw = g_mainWindow;
    mw->DispatcherQueue().TryEnqueue([mw, id]() {
        mw->CloseSurfaceByPaneId(id);
    });
}

}  // namespace winrt::GhosttyWin32::implementation
