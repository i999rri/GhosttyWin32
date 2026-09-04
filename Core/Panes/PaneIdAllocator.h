#pragma once

#include <Panes/PaneId.h>
#include <atomic>

namespace core::panes {

// Issues monotonically increasing PaneIds. One instance lives on App
// (process-wide) so every leaf across every MainWindow gets a unique
// id — that's what makes `close_surface_cb`'s PaneId-in-userdata scheme
// address the exact pane instead of a "first window's pane N" collision.
// The `PaneId` type's uint64 value space is large enough that overflow
// isn't a concern in practice.
//
// Move-disabled: fixed location for App's lifetime.
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

}  // namespace core::panes
