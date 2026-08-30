#pragma once

#include "Ghostty/Actions/Tags/BackgroundOpacity.h"
#include <type_traits>

namespace winrt::GhosttyWin32::implementation {

// The window-scoped action state a window is born with. A window
// created from scratch starts at the defaults; a window created to
// host a torn-out tab starts with a copy of the source window's
// (upstream macOS keeps this state on the controller, which moves
// with the tab).
//
// One field per tag that travels. Adding a tag here is the whole
// change: MainWindow holds its copy as this one value and hands it
// over as a unit, and App::CreateTearOutWindow requires one, so no
// spawn path can forget it.
//
// Only values belong here — state that means the same thing in
// another window. Tags that hold a window's own resources (an HWND,
// a WM_SIZING subclass, a saved placement) are non-copyable, so
// putting one here fails to compile; the assert below says why.
//
// Tags deliberately not here:
//   Fullscreen        a new window is never born fullscreen (and it
//                     holds the pre-fullscreen placement)
//   SizeLimit         re-reported by the surfaces once they present
//   CellSize          re-queried at adopt time (#155)
//   WindowDecorations per window today; one line here if it should
//                     travel
struct WindowState {
    core::ghostty::actions::tags::BackgroundOpacity backgroundOpacity;
};

static_assert(std::is_copy_assignable_v<WindowState>,
              "WindowState travels between windows by copy: only tags "
              "that are plain values (no HWND / subclass / placement) "
              "can go in it");

}  // namespace winrt::GhosttyWin32::implementation
