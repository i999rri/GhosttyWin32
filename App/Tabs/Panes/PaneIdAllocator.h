#pragma once

#include "Tabs/Panes/PaneId.h"
#include <atomic>

namespace winrt::GhosttyWin32::implementation {

// Issues monotonically increasing PaneIds. One instance per MainWindow —
// the counter is per-allocator (not process-global) so that test setups
// or future multi-window scenarios get an isolated ID space without
// having to clear hidden static state.
//
// Move-disabled: fixed location for the lifetime of MainWindow.
class PaneIdAllocator {
public:
    PaneIdAllocator() = default;
    PaneIdAllocator(const PaneIdAllocator&) = delete;
    PaneIdAllocator& operator=(const PaneIdAllocator&) = delete;
    PaneIdAllocator(PaneIdAllocator&&) = delete;
    PaneIdAllocator& operator=(PaneIdAllocator&&) = delete;

    PaneId Allocate() noexcept {
        return PaneId{ m_next.fetch_add(1, std::memory_order_relaxed) };
    }

private:
    std::atomic<uint64_t> m_next{ 1 };  // 0 reserved as sentinel by PaneId
};

}  // namespace winrt::GhosttyWin32::implementation
