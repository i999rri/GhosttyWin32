#pragma once

#include "ghostty.h"

namespace core::ghostty {

class IGhosttyRuntime;

// Builds the `ghostty_runtime_config_s` that `ghostty_app_new` consumes.
// Wires every callback to a small static thunk that finds the
// `IGhosttyRuntime*` (via `rtConfig.userdata` for wakeup,
// `ghostty_app_userdata` for the action callback, and a latched
// file-scope pointer for the surface-scoped callbacks — see below)
// and forwards into the typed methods.
//
// The factory has no App-side dependency; the only thing it knows about
// is `IGhosttyRuntime`, which lives in Core. The provided `runtime`
// must outlive the `ghostty_app_t` created from the returned config —
// see IGhosttyRuntime's lifetime contract.
class RuntimeConfigFactory {
public:
    static ghostty_runtime_config_s Build(IGhosttyRuntime* runtime);

private:
    // Thunks. Each one extracts the runtime from the appropriate
    // ghostty-provided pointer and delegates to a method on the
    // interface. They live as named statics rather than inline
    // lambdas inside `Build()` so each callback's bridging shape is
    // visible at the same altitude as its interface counterpart.
    static void Wakeup(void* userdata);
    static bool Action(ghostty_app_t app,
                       ghostty_target_s target,
                       ghostty_action_s action);
    // The clipboard and close_surface callbacks are surface-scoped in
    // libghostty: their first parameter is the requesting surface's
    // userdata (the PaneId the host stored on surface_config), NOT
    // the rtConfig userdata, so we can't reach the runtime through it
    // directly. `Build()` stashes the runtime pointer in a file-scope
    // atomic that these thunks read; the pane userdata is forwarded
    // so the impl can resolve the exact surface. The pointer stays
    // valid as long as the runtime outlives ghostty_app_free's
    // surface-thread join — the standard member ordering on App
    // guarantees that.
    static bool ReadClipboard(void* paneIdUserdata,
                              ghostty_clipboard_e kind,
                              void* state);
    static void ConfirmReadClipboard(void* paneIdUserdata,
                                     char const* content,
                                     void* state,
                                     ghostty_clipboard_request_e request);
    static void WriteClipboard(void* paneIdUserdata,
                               ghostty_clipboard_e kind,
                               ghostty_clipboard_content_s const* content,
                               size_t count,
                               bool confirmed);
    static void CloseSurface(void* paneIdUserdata, bool process_alive);
};

}  // namespace core::ghostty
