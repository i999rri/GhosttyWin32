#include "Ghostty/RuntimeConfigFactory.h"
#include "Ghostty/IGhosttyRuntime.h"

#include <atomic>

namespace core::ghostty {

namespace {

// Latched on every Build() call. The close_surface_cb signature
// doesn't include runtime userdata — ghostty hands us the per-surface
// userdata (PaneId) instead — so we stash the runtime pointer here
// for that one thunk to read.
//
// Safe because the runtime always outlives ghostty_app_free's
// surface-thread join (the standard member ordering on App).
std::atomic<IGhosttyRuntime*> g_runtimeForCloseSurface{ nullptr };

}  // namespace

ghostty_runtime_config_s RuntimeConfigFactory::Build(IGhosttyRuntime* runtime)
{
    g_runtimeForCloseSurface.store(runtime, std::memory_order_release);

    ghostty_runtime_config_s rtConfig{};
    rtConfig.userdata                  = runtime;
    rtConfig.wakeup_cb                 = &Wakeup;
    rtConfig.action_cb                 = &Action;
    rtConfig.read_clipboard_cb         = &ReadClipboard;
    rtConfig.confirm_read_clipboard_cb = &ConfirmReadClipboard;
    rtConfig.write_clipboard_cb        = &WriteClipboard;
    rtConfig.close_surface_cb          = &CloseSurface;
    return rtConfig;
}

void RuntimeConfigFactory::Wakeup(void* userdata)
{
    static_cast<IGhosttyRuntime*>(userdata)->OnWakeup();
}

bool RuntimeConfigFactory::Action(ghostty_app_t app,
                                  ghostty_target_s target,
                                  ghostty_action_s action)
{
    auto* runtime =
        static_cast<IGhosttyRuntime*>(ghostty_app_userdata(app));
    return runtime->OnAction(target, action);
}

bool RuntimeConfigFactory::ReadClipboard(void* userdata,
                                          ghostty_clipboard_e,
                                          void* state)
{
    return static_cast<IGhosttyRuntime*>(userdata)->OnReadClipboard(state);
}

void RuntimeConfigFactory::ConfirmReadClipboard(void* userdata,
                                                 char const* content,
                                                 void* state,
                                                 ghostty_clipboard_request_e)
{
    static_cast<IGhosttyRuntime*>(userdata)
        ->OnConfirmReadClipboard(content, state);
}

void RuntimeConfigFactory::WriteClipboard(void* userdata,
                                           ghostty_clipboard_e,
                                           ghostty_clipboard_content_s const* content,
                                           size_t count,
                                           bool)
{
    if (!content || count == 0 || !content[0].data) return;
    static_cast<IGhosttyRuntime*>(userdata)->OnWriteClipboard(content[0].data);
}

void RuntimeConfigFactory::CloseSurface(void* paneIdUserdata, bool /*process_alive*/)
{
    if (!paneIdUserdata) return;
    auto* runtime =
        g_runtimeForCloseSurface.load(std::memory_order_acquire);
    if (!runtime) return;
    runtime->OnCloseSurface(paneIdUserdata);
}

}  // namespace core::ghostty
