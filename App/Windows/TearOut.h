#pragma once

#include "Windows/WindowState.h"
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <windows.h>
#include <functional>
#include <optional>

namespace winrt::GhosttyWin32::implementation {

struct MainWindow;

// A tab leaving its window for a new one, browser-style. The one
// place that knows the steps and their order: spawn the host with
// the source window's WindowState, move the tab across, place the
// host at the drop point, hand it focus — and what happens when a
// step can't (the only tab moves its window instead; a host that
// received nothing is closed, not leaked).
//
// It creates no window itself and reaches for no global: the one
// thing it needs from outside — a host window born with the given
// WindowState — is handed in as `spawnHost` (App, which owns the
// window list, provides it).
class TearOut {
public:
    using SpawnHost = std::function<MainWindow*(WindowState const& inherited)>;

    // Move `item`'s tab out of `source` into a fresh window from
    // `spawnHost`. `dropPoint` is the pointer position in screen
    // coordinates, if known; the host is placed so its tab strip
    // lands near it. Returns the new host, or null when nothing
    // moved.
    static MainWindow* ToNewWindow(
        MainWindow& source,
        winrt::Microsoft::UI::Xaml::Controls::TabViewItem const& item,
        std::optional<POINT> dropPoint,
        SpawnHost const& spawnHost);
};

}  // namespace winrt::GhosttyWin32::implementation
