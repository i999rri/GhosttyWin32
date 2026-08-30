#pragma once

#include "Ghostty/Actions/Tags/BackgroundOpacity.h"
#include "Ghostty/Actions/Tags/CellSize.h"
#include "Ghostty/Actions/Tags/Fullscreen.h"
#include "Ghostty/Actions/Tags/SizeLimit.h"
#include "Ghostty/Actions/Tags/WindowDecorations.h"
#include <type_traits>

namespace winrt::GhosttyWin32::implementation {

// Every window-scoped ghostty action tag a MainWindow holds, in one
// place. All of them are plain values — the tags decide, and
// win32::NativeWindow (for the size rules and fullscreen) or the
// XAML side (for the backdrop and the chrome row) carries the
// decision out — so the only question left is which of them a window
// born for another window's tab should start from.
//
//   Inherited  what the user has set on the window: the background-
//              opacity mode and the chrome override. A window created
//              from scratch starts at the defaults; the host of a
//              torn-out tab, or the window that stands in for a new
//              tab when the chrome is hidden, starts with a copy of
//              the source's so it looks like where it came from
//              (upstream macOS keeps this state on the controller,
//              which moves with the tab).
//
//   the rest   what describes this window: the size limit its
//              surfaces reported, the cell metrics it measured,
//              whether it is in fullscreen. A new window learns its
//              own — surfaces re-report SIZE_LIMIT, the adopt path
//              re-queries CELL_SIZE (#155, the DPI may differ) — and
//              is never born fullscreen.
//
// Adding a tag is one line here; adding it to Inherited is the whole
// change for making it travel, since App's window factories take an
// Inherited and copy it as a unit.
struct WindowState {
    struct Inherited {
        core::ghostty::actions::tags::BackgroundOpacity backgroundOpacity;
        // Not a tear-out concern (an undecorated window has no tab
        // strip to drag from), but it is what makes "new tab → new
        // window" come up looking like the window that asked for it.
        core::ghostty::actions::tags::WindowDecorations windowDecorations;
    };

    Inherited inherited;
    core::ghostty::actions::tags::SizeLimit  sizeLimit;
    core::ghostty::actions::tags::CellSize   cellSize;
    core::ghostty::actions::tags::Fullscreen fullscreen;
};

static_assert(std::is_copy_assignable_v<WindowState::Inherited>,
              "WindowState::Inherited travels between windows by copy");

}  // namespace winrt::GhosttyWin32::implementation
