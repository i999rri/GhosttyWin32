#pragma once

#include "TabId.h"
#include <atomic>

namespace winrt::GhosttyWin32::implementation {

// Issues monotonically increasing TabIds. One instance per MainWindow —
// the counter is per-allocator (not process-global) so that test setups
// or future multi-window scenarios get an isolated ID space without
// having to clear hidden static state.
//
// Move-disabled: fixed location for the lifetime of MainWindow.
class TabIdAllocator {
public:
    TabIdAllocator() = default;
    TabIdAllocator(const TabIdAllocator&) = delete;
    TabIdAllocator& operator=(const TabIdAllocator&) = delete;
    TabIdAllocator(TabIdAllocator&&) = delete;
    TabIdAllocator& operator=(TabIdAllocator&&) = delete;

    TabId Allocate() noexcept {
        return TabId{ m_next.fetch_add(1, std::memory_order_relaxed) };
    }

private:
    std::atomic<uint64_t> m_next{ 1 };  // 0 reserved as sentinel by TabId
};

}  // namespace winrt::GhosttyWin32::implementation
