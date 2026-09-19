#include "Ghostty/RuntimeConfigFactory.h"
#include "Ghostty/IGhosttyRuntime.h"

#include <atomic>

namespace core::ghostty {

namespace {

// Latched on every Build() call. The surface-scoped callbacks
// (close_surface and all three clipboard hooks) don't receive the
// runtime userdata — ghostty hands them the per-surface userdata
// (PaneId) instead — so those thunks read the runtime pointer from
// here and forward the surface userdata to the interface.
//
// Safe because the runtime always outlives ghostty_app_free's
// surface-thread join (the standard member ordering on App).
std::atomic<IGhosttyRuntime*> g_runtimeForSurfaceCallbacks{ nullptr };

}  // namespace

ghostty_runtime_config_s RuntimeConfigFactory::Build(IGhosttyRuntime* runtime)
{
    g_runtimeForSurfaceCallbacks.store(runtime, std::memory_order_release);

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

// The clipboard callbacks are surface-scoped in libghostty: the first
// parameter is the requesting surface's userdata (embedded.zig passes
// `self.userdata`, i.e. the value the host set in surface_config —
// a PaneId on this host), NOT the runtime userdata. Casting it to
// IGhosttyRuntime* would virtual-call through a PaneId. Route through
// the latched runtime instead and forward the pane userdata so the
// impl can complete the request on the exact surface that issued it.

ghostty_clipboard_read_result_e RuntimeConfigFactory::ReadClipboard(
    void* paneIdUserdata,
    ghostty_clipboard_e,
    void* state,
    char const* const* mimes,
    size_t mimesLen,
    bool list)
{
    auto* runtime =
        g_runtimeForSurfaceCallbacks.load(std::memory_order_acquire);
    if (!runtime) return GHOSTTY_CLIPBOARD_READ_UNSUPPORTED;
    return runtime->OnReadClipboard(paneIdUserdata, state, mimes, mimesLen, list);
}

void RuntimeConfigFactory::ConfirmReadClipboard(void* paneIdUserdata,
                                                 ghostty_clipboard_confirm_s const* confirm,
                                                 void* state,
                                                 ghostty_clipboard_request_e request)
{
    auto* runtime =
        g_runtimeForSurfaceCallbacks.load(std::memory_order_acquire);
    if (!runtime) return;
    runtime->OnConfirmReadClipboard(paneIdUserdata, confirm, state, request);
}

// The trailing bool asks for a confirmation prompt before writing;
// this host has none and applies the write as it always has.
void RuntimeConfigFactory::WriteClipboard(void* paneIdUserdata,
                                           ghostty_clipboard_e,
                                           ghostty_clipboard_content_s const* contents,
                                           size_t count,
                                           bool)
{
    if (!contents || count == 0) return;
    auto* runtime =
        g_runtimeForSurfaceCallbacks.load(std::memory_order_acquire);
    if (!runtime) return;
    runtime->OnWriteClipboard(paneIdUserdata, contents, count);
}

void RuntimeConfigFactory::CloseSurface(void* paneIdUserdata, bool /*process_alive*/)
{
    if (!paneIdUserdata) return;
    auto* runtime =
        g_runtimeForSurfaceCallbacks.load(std::memory_order_acquire);
    if (!runtime) return;
    runtime->OnCloseSurface(paneIdUserdata);
}

}  // namespace core::ghostty
