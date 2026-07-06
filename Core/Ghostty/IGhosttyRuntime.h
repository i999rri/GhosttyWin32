#pragma once

#include "ghostty.h"

namespace core::ghostty {

// The host-facing side of ghostty's runtime callbacks. RuntimeConfigFactory
// builds a `ghostty_runtime_config_s` whose C function pointers do the
// minimum bridging work — they unwrap the runtime pointer ghostty hands
// back via userdata and dispatch into the methods on this interface.
//
// Splitting it out this way buys two things:
//
//   1. RuntimeConfigFactory lives in Core. The factory only references
//      this interface and ghostty.h; it has no MainWindow, App, or other
//      App-side dependencies, so the C↔C++ translation layer can be
//      tested in isolation.
//
//   2. The host's runtime implementation (MainWindowRuntime in App) is
//      a plain virtual subclass. Lifecycle defence, win32 clipboard
//      use, dispatcher routing — everything that has to know about
//      this specific host — lives in one place behind a typed
//      interface, instead of being scattered across six C function
//      pointer bodies.
//
// Ownership / lifetime contract: the implementation must outlive the
// `ghostty_app_t` created from the produced rtConfig. The standard
// pattern is to declare the runtime as an App member before the
// ghostty wrapper, so destruction runs in the safe order:
//
//   1. window member destructed → MainWindow gone → surfaces freed.
//   2. ghostty wrapper destructed → ghostty_app_free joins worker
//      threads. Any in-flight callback from a thread that hasn't
//      observed the join yet still finds the runtime alive.
//   3. runtime member destructed. No more callbacks can fire by here.
class IGhosttyRuntime {
public:
    virtual ~IGhosttyRuntime() = default;

    // ghostty asks the host to drive its event loop forward. Fired from
    // a worker thread; impl is expected to hop to the UI thread before
    // calling `ghostty_app_tick`.
    virtual void OnWakeup() = 0;

    // A keybind or escape-sequence-driven action — new_tab, close_tab,
    // toggle_fullscreen, set_title, … Target indicates whether the
    // action is app-wide or scoped to a specific surface. Returns true
    // when the host has handled it.
    virtual bool OnAction(ghostty_target_s target,
                          ghostty_action_s action) = 0;

    // Terminal-initiated clipboard read. `state` is opaque ghostty
    // bookkeeping the impl hands back to
    // `ghostty_surface_complete_clipboard_request` once the OS
    // clipboard has been read. Returns true when the host completed
    // the request (false leaves the request unresolved).
    virtual bool OnReadClipboard(void* state) = 0;

    // Confirmation step for a previously-issued read. Ghostty issues
    // this when the read could be unsafe (bracketed paste with
    // newlines etc.); the impl decides whether to accept and then
    // completes the request.
    virtual void OnConfirmReadClipboard(char const* content,
                                        void* state) = 0;

    // Terminal-initiated clipboard write — typically an OSC 52 from a
    // tmux / shell helper. The UTF-8 payload is non-null and non-empty
    // by the time the factory invokes this.
    virtual void OnWriteClipboard(char const* utf8) = 0;

    // Shell exited or ghostty otherwise asks the host to close the
    // surface. `paneIdUserdata` is the per-surface userdata the host
    // set in surface_config — typically a PaneId encoded as void*.
    virtual void OnCloseSurface(void* paneIdUserdata) = 0;
};

}  // namespace core::ghostty
