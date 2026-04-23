#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "ghostty.h"

// Build ghostty modifier flags from individual key states.
inline ghostty_input_mods_e buildMods(bool shift, bool ctrl, bool alt) {
    int mods = 0;
    if (shift) mods |= GHOSTTY_MODS_SHIFT;
    if (ctrl)  mods |= GHOSTTY_MODS_CTRL;
    if (alt)   mods |= GHOSTTY_MODS_ALT;
    return static_cast<ghostty_input_mods_e>(mods);
}

// Build ghostty modifier flags from current Win32 key state.
inline ghostty_input_mods_e currentMods() {
    return buildMods(
        GetKeyState(VK_SHIFT) & 0x8000,
        GetKeyState(VK_CONTROL) & 0x8000,
        GetKeyState(VK_MENU) & 0x8000
    );
}
