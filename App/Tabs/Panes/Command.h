#pragma once

#include <Panes/Command.h>

namespace winrt::GhosttyWin32::implementation {

// The model type lives in Core (Core/Panes/Command.h) so the pane
// tree and its tests can use it without this project; the alias
// keeps the App's spelling short.
using Command = core::panes::Command;

}  // namespace winrt::GhosttyWin32::implementation
