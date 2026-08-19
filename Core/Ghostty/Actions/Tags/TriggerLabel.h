#pragma once

#include "ghostty.h"
#include <string>

namespace core::ghostty::actions {

// Human-readable label for a keybind trigger, in ghostty's own
// keybind config syntax (e.g. "ctrl+shift+a", "ctrl+bracket_left").
// Used by the KEY_SEQUENCE indicator so the pending chord on screen
// reads exactly like the line the user wrote in their config.
//
// Pure function — no WinUI, no state — so the mods ordering and the
// physical-key name table are unit-testable without a window.
std::wstring TriggerLabel(ghostty_input_trigger_s trigger);

}  // namespace core::ghostty::actions
