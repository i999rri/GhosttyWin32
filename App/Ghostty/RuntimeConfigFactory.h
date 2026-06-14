#pragma once

#include "ghostty.h"

namespace winrt::GhosttyWin32::implementation {

// Builds the `ghostty_runtime_config_s` that `ghostty::App::Create`
// expects — wakeup / action / clipboard / close_surface callbacks.
//
// Every callback is a C function pointer with no capture, so it has
// to reach host state through statics (`g_mainWindow`, `App::g_app`).
// That means the construction has no instance dependency: App can
// call `Build()` from `OnLaunched` *before* the first MainWindow has
// been constructed and feed the result straight into
// `core::ghostty::App::Create`. The first callback won't fire until
// a surface exists, by which point the Activated handler has set
// `g_mainWindow` and everything is wired.
//
// Each callback lives as a named static member rather than as an
// inline lambda inside `Build()` so the intent is visible at a
// glance — `Build()` reads as a manifest of "what hooks ghostty
// gets," and each behaviour is documented at its own definition.
class RuntimeConfigFactory {
public:
    static ghostty_runtime_config_s Build();

private:
    // Worker thread → UI thread → `ghostty_app_tick`. Drives ghostty's
    // event loop forward when libghostty asks us to wake up.
    static void Wakeup(void* userdata);

    // Forwards the action — keybind triggers like `new_tab`,
    // `toggle_fullscreen`, `reset_window_size` — into the host-side
    // dispatcher, which fans it out to the right handler.
    static bool Action(ghostty_app_t app,
                       ghostty_target_s target,
                       ghostty_action_s action);

    // Terminal-initiated clipboard read (e.g. a paste from the
    // terminal's own keybind). Pulls UTF-16 from Win32's clipboard,
    // re-encodes to UTF-8, and feeds the result back through
    // `Surface::CompleteClipboardRequest`.
    static bool ReadClipboard(void* userdata,
                              ghostty_clipboard_e kind,
                              void* state);

    // Auto-confirm path for the clipboard-read confirmation step.
    // Ghostty asks first when the read could be unsafe (e.g. bracketed
    // paste with newlines); we always accept — the host doesn't show
    // a dialog here.
    static void ConfirmReadClipboard(void* userdata,
                                     char const* content,
                                     void* state,
                                     ghostty_clipboard_request_e request);

    // Terminal-initiated clipboard write — typically OSC 52 from a
    // tmux / shell helper. Converts UTF-8 to UTF-16 and writes via
    // the Win32 clipboard.
    static void WriteClipboard(void* userdata,
                               ghostty_clipboard_e kind,
                               ghostty_clipboard_content_s const* content,
                               size_t count,
                               bool confirmed);

    // Shell exited (`exit`) or ghostty otherwise wants the surface
    // gone. Userdata is the `PaneId` we stashed in
    // `TabFactory::MakeLeaf`; we hop to the UI thread and either
    // close the tab (if it was the only pane) or collapse the split.
    static void CloseSurface(void* userdata, bool process_alive);
};

}  // namespace winrt::GhosttyWin32::implementation
