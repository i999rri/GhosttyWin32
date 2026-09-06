#pragma once

#include <cstdint>

namespace core::panes {

// Strongly-typed pane identifier. Wraps a uint64_t so that
//   - random uint64_t values (DPI, sizes, counts) can't be mistakenly
//     passed as a PaneId
//   - the void* boundary with ghostty's cfg.userdata is centralized in
//     ToUserdata / FromUserdata, instead of bare reinterpret_casts
//     scattered through the host
//
// Allocated once per leaf in TabFactory and threaded through
// cfg.userdata so close_surface_cb can route back to the right pane.
// Every leaf gets its own ID, so close_surface_cb can identify the
// specific pane without ambiguity.
//
// Value 0 is reserved as a sentinel meaning "no ID" (default-constructed
// PaneId converts to false).
struct PaneId {
    uint64_t value{ 0 };

    constexpr bool operator==(PaneId const&) const noexcept = default;
    explicit constexpr operator bool() const noexcept { return value != 0; }

    // void* boundary helpers. cfg.userdata is opaque to ghostty — it
    // hands the same bits back through close_surface_cb. uintptr_t hop
    // is the portable cast path between integer and pointer.
    void* ToUserdata() const noexcept {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
    }
    static PaneId FromUserdata(void* p) noexcept {
        return PaneId{ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)) };
    }
};

}  // namespace core::panes
