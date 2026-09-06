#pragma once

namespace core::panes {

// What Tree::RemovePane did. Callers key UI reactions off which case
// fired: last-pane removal closes the whole tab, split-collapse just
// relayouts, not-found is a stale close event. Predicate methods so
// call sites read as `result.IsCollapsed()` rather than
// `result == …::Collapsed`.
class RemoveResult {
public:
    static constexpr RemoveResult NotFound()    noexcept { return { Kind::NotFound }; }
    static constexpr RemoveResult Collapsed()   noexcept { return { Kind::Collapsed }; }
    static constexpr RemoveResult RemovedRoot() noexcept { return { Kind::RemovedRoot }; }

    constexpr bool IsNotFound()    const noexcept { return m_kind == Kind::NotFound; }
    constexpr bool IsCollapsed()   const noexcept { return m_kind == Kind::Collapsed; }
    constexpr bool IsRemovedRoot() const noexcept { return m_kind == Kind::RemovedRoot; }

private:
    enum class Kind { NotFound, Collapsed, RemovedRoot };
    constexpr RemoveResult(Kind k) noexcept : m_kind(k) {}
    Kind m_kind;
};

}  // namespace core::panes
