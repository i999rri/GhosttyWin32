#pragma once

#include "ghostty.h"

#include <algorithm>
#include <vector>

namespace winrt::GhosttyWin32::implementation {

class MainWindow;

// Aggregate for the set of MainWindows live in the process. MainWindow
// adds and removes itself as part of its own lifecycle; runtime
// callbacks and target-based routing consult this collection to reach
// the right window for a given surface (or any window when the
// callback has no per-surface handle).
//
// Sibling to `Tabs` (which manages the `Tab*` collection inside a
// MainWindow): both are borrowing collections built out of raw
// pointers into objects whose ownership lives elsewhere. Ownership of
// each MainWindow stays with `winrt::App::window`; ownership of the
// aggregate itself stays with `winrt::App::m_windows`.
//
// Not thread-safe. Registration/lookup all happen on the UI thread —
// MainWindow's ctor/dtor and the runtime callbacks that reach here go
// through the DispatcherQueue hop before touching state.
class MainWindows {
public:
    MainWindows() = default;

    MainWindows(MainWindows const&)            = delete;
    MainWindows& operator=(MainWindows const&) = delete;
    MainWindows(MainWindows&&)                 = delete;
    MainWindows& operator=(MainWindows&&)      = delete;

    void Register(MainWindow* w)
    {
        if (!w) return;
        m_windows.push_back(w);
    }

    void Unregister(MainWindow* w) noexcept
    {
        auto it = std::find(m_windows.begin(), m_windows.end(), w);
        if (it != m_windows.end()) m_windows.erase(it);
    }

    // Any registered window (currently: the first). For callbacks
    // that have no per-surface target — wakeup, clipboard read/write,
    // APP-target actions. Null before any window registers or after
    // all have unregistered.
    MainWindow* Any() const noexcept
    {
        return m_windows.empty() ? nullptr : m_windows.front();
    }

    // Window whose tab tree owns `surface`, or null. Linear scan; the
    // window count in practice is single digits, so no index needed.
    // Definition lives in MainWindows.cpp to break the header include
    // cycle (MainWindow needs to be complete for `OwnsSurface`).
    MainWindow* FindForSurface(ghostty_surface_t surface) const noexcept;

    bool   Empty() const noexcept { return m_windows.empty(); }
    size_t Count() const noexcept { return m_windows.size(); }

private:
    std::vector<MainWindow*> m_windows;
};

}  // namespace winrt::GhosttyWin32::implementation
