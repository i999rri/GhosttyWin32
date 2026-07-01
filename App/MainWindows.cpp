#include "pch.h"
#include "MainWindows.h"

#include "MainWindow.xaml.h"

namespace winrt::GhosttyWin32::implementation {

MainWindow* MainWindows::FindForSurface(ghostty_surface_t surface) const noexcept
{
    if (!surface) return nullptr;
    for (auto* w : m_windows) {
        if (w && w->OwnsSurface(surface)) return w;
    }
    return nullptr;
}

}  // namespace winrt::GhosttyWin32::implementation
