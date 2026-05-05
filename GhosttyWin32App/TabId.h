#pragma once

#include <cstdint>

namespace winrt::GhosttyWin32::implementation {

// Strongly-typed tab identifier. Wraps a uint64_t so that
//   - random uint64_t values (DPI, sizes, counts) can't be mistakenly
//     passed as a TabId
//   - the void* boundary with ghostty's cfg.userdata is centralized in
//     ToUserdata / FromUserdata, instead of bare reinterpret_casts
//     scattered through the host
//
// Value 0 is reserved as a sentinel meaning "no ID" (default-constructed
// TabId converts to false).
struct TabId {
    uint64_t value{ 0 };

    constexpr bool operator==(TabId const&) const noexcept = default;
    explicit constexpr operator bool() const noexcept { return value != 0; }

    // void* boundary helpers. cfg.userdata is opaque to ghostty — it
    // hands the same bits back through close_surface_cb. uintptr_t hop
    // is the portable cast path between integer and pointer.
    void* ToUserdata() const noexcept {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
    }
    static TabId FromUserdata(void* p) noexcept {
        return TabId{ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)) };
    }
};

}  // namespace winrt::GhosttyWin32::implementation
