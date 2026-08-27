#pragma once

#include "ghostty.h"
#include <cstddef>
#include <string>

namespace core::host {

// The host-side view of ONE ghostty surface — what a pane's control
// must be able to do when libghostty sends a surface-targeted action.
//
// Why this exists: ghostty addresses most actions to a surface, but
// the host used to expose only a per-window interface (IWindow), so
// every surface action went "dispatcher → window → look the pane up
// → forward to the control" — fourteen identical five-line relays on
// MainWindow whose only job was the lookup. The window was acting as
// a post office. Now the window is a directory instead: Actions asks
// IWindow::FindSurfaceView(surface) once and talks to the view
// directly. Adding a surface action means one method here and one
// implementation on the control, with no window code at all.
//
// WinUI-agnostic on purpose (std::wstring, plain enums) so Core can
// be compiled and unit-tested without XAML; the App layer's
// TerminalControl implements it. Every method runs on the UI
// thread — Actions dispatches before calling.
//
// Ownership: the pane's control implements this and owns the
// surface. Pointers handed out by FindSurfaceView are borrowed and
// valid only for the duration of the dispatched call in which they
// were obtained (the lookup happens on the UI thread, in the same
// dispatched lambda that uses the result, so the pane cannot be torn
// down in between).
class ISurfaceView {
public:
    virtual ~ISurfaceView() = default;

    // ----- pointer -----

    // MOUSE_SHAPE: ghostty asked for a cursor shape over this pane.
    virtual void SetCursorShape(ghostty_action_mouse_shape_e shape) = 0;

    // MOUSE_VISIBILITY: HIDDEN while the user types (config
    // `mouse-hide-while-typing`), VISIBLE again on the next pointer
    // move. ghostty drives both directions; the host only mirrors.
    virtual void SetMouseVisibility(bool visible) = 0;

    // MOUSE_OVER_LINK: show the hovered-link banner with `url`, or
    // hide it when `url` is empty (upstream fires an empty payload
    // when the pointer leaves the link).
    virtual void SetHoveredLink(std::wstring url) = 0;

    // ----- status indicators -----

    // SECURE_INPUT: ON / OFF from shell integration (password
    // prompt detected / ended), TOGGLE from the toggle_secure_input
    // keybind. The view resolves TOGGLE against its own state — the
    // indicator is pane-visual state, so the pane owns it.
    virtual void SetSecureInput(ghostty_action_secure_input_e mode) = 0;

    // READONLY: indicator only — the pty-write blocking lives in
    // libghostty and already works.
    virtual void SetReadonly(bool readonly) = 0;

    // KEY_SEQUENCE / KEY_TABLE: modal keyboard state, pre-formatted
    // by Core (TriggerLabel) so the view never touches ghostty
    // trigger structs. The pane owns the accumulated state (pending
    // chord list, key-table stack) because that is where it renders.
    virtual void AppendKeySequence(std::wstring triggerLabel) = 0;
    virtual void ClearKeySequence() = 0;
    virtual void PushKeyTable(std::wstring name) = 0;
    virtual void PopKeyTable(bool all) = 0;

    // ----- scrollback -----

    // SCROLLBAR: total rows, viewport offset, viewport length. Fires
    // on every scroll and on screen changes; feeds the overlay
    // scrollbar (#154).
    virtual void SetScrollbar(ghostty_action_scrollbar_s bar) = 0;

    // ----- search bar -----
    // ghostty owns matching and the lifecycle; the view owns the
    // input box and talks back through binding actions
    // (Surface::Search / NavigateSearch / EndSearch). `needle` is
    // empty for a bare open and pre-filled by search_selection.
    // `total` / `selected` are -1 while unknown; `selected` is
    // 1-based.
    virtual void StartSearch(std::wstring needle) = 0;
    virtual void EndSearch() = 0;
    virtual void SetSearchTotal(ptrdiff_t total) = 0;
    virtual void SetSearchSelected(ptrdiff_t selected) = 0;
};

}  // namespace core::host
